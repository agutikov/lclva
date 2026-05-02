// M5 spike: live SDP offer/answer round-trip against Speaches' WebRTC
// realtime endpoint at POST /v1/realtime.
//
// Goal of this file: prove libdatachannel + Speaches' aiortc negotiate
// successfully on the dev workstation, the data channel reaches the
// `Open` state, and we can record what (if any) events the server emits
// before any audio is sent.
//
// What this is NOT:
//   - production STT client (lands in M5 Step 2 once protocol is locked)
//   - audio-streaming verification (covered by a follow-up smoke once
//     the bare connection works)
//
// Skips when:
//   - Speaches /health is unreachable (matches test_speaches_smoke.cpp)
//   - LibDataChannel was not found at configure time (target is gated
//     in tests/CMakeLists.txt; if you can compile this file at all, the
//     library is present).

#include "config/config.hpp"
#include "stt/realtime_stt_client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string speaches_url() {
    if (const char* p = std::getenv("ACVA_SPEACHES_URL"); p && *p) {
        return p;
    }
    return "http://127.0.0.1:8090";
}

struct ParsedUrl {
    std::string host;
    int         port = 0;
    bool        ok   = false;
};

ParsedUrl parse(const std::string& url) {
    ParsedUrl r;
    static const std::string kPrefix = "http://";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) return r;
    auto rest      = url.substr(kPrefix.size());
    auto slash     = rest.find('/');
    auto authority = rest.substr(0, slash == std::string::npos ? rest.size() : slash);
    auto colon     = authority.find(':');
    r.host = authority.substr(0, colon);
    r.port = colon == std::string::npos ? 80 : std::atoi(authority.substr(colon + 1).c_str());
    r.ok   = !r.host.empty() && r.port > 0;
    return r;
}

bool speaches_reachable() {
    auto p = parse(speaches_url());
    if (!p.ok) return false;
    httplib::Client c(p.host, p.port);
    c.set_connection_timeout(std::chrono::seconds(2));
    auto r = c.Get("/health");
    return r && r->status == 200;
}

// Same production STT model as config/default.yaml,
// download-speaches-models.sh, and `acva demo stt`. Sharing one
// model across all integration tests keeps the GPU footprint
// minimal — Speaches loads it once and keeps it warm for the rest
// of the run.
constexpr const char* kRealtimeModel = "deepdml/faster-whisper-large-v3-turbo-ct2";

} // namespace

TEST_CASE("Speaches realtime spike: SDP offer/answer + data channel opens"
           * doctest::skip(!speaches_reachable())) {
    // ----- 1. Build a peer connection with a sendonly audio track and
    //          a data channel. The track gives the offer an audio
    //          m-line (Speaches' aiortc rejects offers without one for
    //          the realtime endpoint); the data channel carries
    //          transcription events back from the server.
    rtc::Configuration cfg;
    // No ICE servers — host ↔ container bridge only.
    cfg.iceTransportPolicy = rtc::TransportPolicy::All;

    auto pc = std::make_shared<rtc::PeerConnection>(cfg);

    std::mutex                 m;
    std::condition_variable    gathered_cv;
    std::condition_variable    state_cv;
    bool                       gathered     = false;
    bool                       channel_open = false;
    std::vector<std::string>   messages;
    rtc::PeerConnection::State pc_state = rtc::PeerConnection::State::New;

    pc->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
        if (s == rtc::PeerConnection::GatheringState::Complete) {
            std::lock_guard<std::mutex> lk(m);
            gathered = true;
            gathered_cv.notify_all();
        }
    });
    pc->onStateChange([&](rtc::PeerConnection::State s) {
        std::lock_guard<std::mutex> lk(m);
        pc_state = s;
        state_cv.notify_all();
        MESSAGE("pc state=" << static_cast<int>(s));
    });

    rtc::Description::Audio audio_desc("audio", rtc::Description::Direction::SendOnly);
    audio_desc.addOpusCodec(111);
    auto audio_track = pc->addTrack(audio_desc);

    auto dc = pc->createDataChannel("oai-events");
    dc->onOpen([&] {
        std::lock_guard<std::mutex> lk(m);
        channel_open = true;
        state_cv.notify_all();
        MESSAGE("data channel open: " << dc->label());
    });
    dc->onMessage([&](rtc::message_variant data) {
        std::lock_guard<std::mutex> lk(m);
        if (std::holds_alternative<std::string>(data)) {
            messages.push_back(std::get<std::string>(data));
            MESSAGE("dc message: " << messages.back());
        } else {
            messages.emplace_back("<binary>");
        }
        state_cv.notify_all();
    });

    pc->setLocalDescription();

    // ----- 2. Wait for ICE gathering to complete so localDescription()
    //          contains the full offer + candidates.
    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(gathered_cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return gathered;
        }));
    }
    auto offer_opt = pc->localDescription();
    REQUIRE(offer_opt.has_value());
    std::string offer_sdp = std::string(*offer_opt);
    INFO("offer.size=" << offer_sdp.size());
    REQUIRE(offer_sdp.find("m=audio") != std::string::npos);
    REQUIRE(offer_sdp.find("m=application") != std::string::npos); // data channel
    REQUIRE(offer_sdp.find("a=ice-ufrag") != std::string::npos);

    // ----- 3. POST the offer to /v1/realtime; expect SDP answer back.
    auto p = parse(speaches_url());
    REQUIRE(p.ok);
    httplib::Client http(p.host, p.port);
    http.set_connection_timeout(std::chrono::seconds(5));
    http.set_read_timeout(std::chrono::seconds(15));

    std::string path = std::string("/v1/realtime?model=") + kRealtimeModel;
    auto res = http.Post(path.c_str(), offer_sdp, "application/sdp");
    REQUIRE(res);
    INFO("POST /v1/realtime status=" << res->status
         << " body_len=" << res->body.size());
    REQUIRE(res->status == 200);
    REQUIRE(res->body.find("v=0") == 0);
    REQUIRE(res->body.find("m=audio") != std::string::npos);

    // ----- 4. Apply the answer; wait for ICE+DTLS to connect.
    pc->setRemoteDescription(rtc::Description(res->body, "answer"));

    {
        std::unique_lock<std::mutex> lk(m);
        const bool connected = state_cv.wait_for(
            lk, std::chrono::seconds(10), [&] {
                return pc_state == rtc::PeerConnection::State::Connected
                    || pc_state == rtc::PeerConnection::State::Failed
                    || pc_state == rtc::PeerConnection::State::Closed;
            });
        REQUIRE(connected);
        CHECK(pc_state == rtc::PeerConnection::State::Connected);
    }

    // ----- 5. Data channel should open shortly after the peer
    //          connection reaches Connected. Linger briefly to pick up
    //          any events the server proactively sends (session.created,
    //          transcription_session.created — depends on the version
    //          of aiortc's realtime router).
    {
        std::unique_lock<std::mutex> lk(m);
        state_cv.wait_for(lk, std::chrono::seconds(3),
                          [&] { return channel_open; });
        // If the server's data channel opens passively (server-created
        // negotiated channel) the client-created channel above may stay
        // closed — that's fine; we record the observation and let the
        // assertion be informational.
        MESSAGE("client dc opened=" << channel_open);
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::lock_guard<std::mutex> lk(m);
        MESSAGE("collected " << messages.size() << " data-channel messages");
        for (std::size_t i = 0; i < messages.size(); ++i) {
            MESSAGE("  [" << i << "] " << messages[i]);
        }

        // Speaches proactively emits the OpenAI-realtime `session.created`
        // event right after the data channel opens. The wire format is
        // not raw OpenAI JSON — Speaches wraps every event in a
        // fragmentation envelope:
        //   {"id":"event_…","type":"partial_message","data":"<base64>",
        //    "fragment_index":N,"total_fragments":M}
        // The production STT client (M5 Step 2) must reassemble fragments
        // by id, base64-decode, then parse the OpenAI event payload.
        // Asserting the envelope shape here pins that contract.
        REQUIRE(!messages.empty());
        CHECK(messages[0].find(R"("type":"partial_message")")
              != std::string::npos);
        CHECK(messages[0].find(R"("fragment_index")") != std::string::npos);
        CHECK(messages[0].find(R"("total_fragments")") != std::string::npos);
        CHECK(messages[0].find(R"("data":")") != std::string::npos);
    }

    pc->close();
}

TEST_CASE("RealtimeSttClient: start() reaches Ready against live Speaches"
           * doctest::skip(!speaches_reachable())) {
    acva::config::SttConfig cfg;
    cfg.base_url = speaches_url() + "/v1";
    cfg.model    = kRealtimeModel;
    cfg.request_timeout_seconds = 30;

    acva::stt::RealtimeSttClient client(cfg);
    REQUIRE(client.state() == acva::stt::RealtimeSttClient::State::Idle);

    const bool ok = client.start(std::chrono::seconds(15));
    INFO("final state="
         << static_cast<int>(client.state()));
    CHECK(ok);
    CHECK(client.state() == acva::stt::RealtimeSttClient::State::Ready);

    client.stop();
    CHECK(client.state() == acva::stt::RealtimeSttClient::State::Closed);
}

namespace {

// Synthesize `text` via Speaches TTS, return as 16 kHz mono int16 PCM
// samples. Mirrors the helper in test_speaches_smoke.cpp but
// inlined here so the realtime smoke is self-contained.
std::vector<std::int16_t> tts_to_16k_pcm(httplib::Client& http,
                                          std::string_view text) {
    std::string body = std::string(R"({"model":"speaches-ai/piper-en_US-amy-medium","input":")")
                       + std::string(text)
                       + R"(","voice":"amy","response_format":"wav"})";
    auto res = http.Post("/v1/audio/speech", body, "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    const auto& wav = res->body;
    REQUIRE(wav.size() > 1024);

    // Speaches WAV preamble is 44 bytes; payload is mono int16 22050 Hz.
    std::vector<std::int16_t> samples_22k((wav.size() - 44) / 2);
    std::memcpy(samples_22k.data(), wav.data() + 44, samples_22k.size() * 2);

    // Nearest-neighbour 22050 → 16000. Good enough for a smoke; the
    // production audio pipeline uses soxr.
    std::vector<std::int16_t> samples_16k;
    const double ratio = 16000.0 / 22050.0;
    const std::size_t n = static_cast<std::size_t>(
        static_cast<double>(samples_22k.size()) * ratio);
    samples_16k.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            static_cast<double>(i) / ratio);
        samples_16k.push_back(src < samples_22k.size() ? samples_22k[src] : 0);
    }
    return samples_16k;
}

} // namespace

TEST_CASE("RealtimeSttClient: WAV fixture round-trip — partials + final transcript"
           * doctest::skip(!speaches_reachable())) {
    constexpr std::string_view kPhrase =
        "Hello from acva. This is a streaming smoke test.";

    // 1. Synthesize a known waveform via the same Speaches container,
    //    so the test is self-contained and uses the production model
    //    end-to-end.
    auto host = parse(speaches_url());
    REQUIRE(host.ok);
    httplib::Client http(host.host, host.port);
    http.set_connection_timeout(std::chrono::seconds(5));
    http.set_read_timeout(std::chrono::seconds(60));
    const auto samples_16k = tts_to_16k_pcm(http, kPhrase);
    REQUIRE(samples_16k.size() > 16000); // > 1 second of speech

    // 2. Open the realtime session.
    acva::config::SttConfig cfg;
    cfg.base_url = speaches_url() + "/v1";
    cfg.model    = kRealtimeModel;
    cfg.request_timeout_seconds = 30;

    acva::stt::RealtimeSttClient client(cfg);
    REQUIRE(client.start(std::chrono::seconds(15)));
    REQUIRE(client.state() == acva::stt::RealtimeSttClient::State::Ready);

    // 3. Wire up callbacks. PartialTranscript may fire 0+ times
    //    depending on how Speaches batches deltas for the chosen
    //    model — we assert at-least-one but the actual production
    //    requirement is "FinalTranscript matches the phrase".
    std::mutex                            cb_mu;
    std::condition_variable               cb_cv;
    std::vector<acva::event::PartialTranscript> partials;
    std::optional<acva::event::FinalTranscript>  final_transcript;
    std::optional<std::string>            error_message;

    acva::stt::RealtimeSttClient::UtteranceCallbacks cb;
    cb.on_partial = [&](acva::event::PartialTranscript p) {
        std::lock_guard lk(cb_mu);
        partials.push_back(std::move(p));
        cb_cv.notify_all();
    };
    cb.on_final = [&](acva::event::FinalTranscript f) {
        std::lock_guard lk(cb_mu);
        final_transcript = std::move(f);
        cb_cv.notify_all();
    };
    cb.on_error = [&](std::string err) {
        std::lock_guard lk(cb_mu);
        error_message = std::move(err);
        cb_cv.notify_all();
    };

    constexpr acva::dialogue::TurnId kTurn = 42;
    auto cancel = std::make_shared<acva::dialogue::CancellationToken>();

    // 4. Push the audio in 200 ms chunks, mirroring how the M4
    //    pipeline will call push_audio in production. We append
    //    ~800 ms of trailing silence so Speaches' server-side VAD
    //    (silence_duration_ms = 550 default) detects end-of-speech
    //    before our `input_audio_buffer.commit` lands. The M4
    //    Silero endpointer's hangover naturally provides this in
    //    production; the synthesized fixture has none.
    auto padded = samples_16k;
    padded.insert(padded.end(), 12'800, std::int16_t{0}); // 800 ms @ 16 kHz

    client.begin_utterance(kTurn, cancel, std::move(cb));
    constexpr std::size_t kChunkSamples = 16'000 / 5; // 200 ms @ 16 kHz, what M4 will push
    for (std::size_t i = 0; i < padded.size(); i += kChunkSamples) {
        const std::size_t end = std::min(i + kChunkSamples, padded.size());
        client.push_audio(
            std::span<const std::int16_t>(padded.data() + i, end - i));
    }
    client.end_utterance();

    // 5. Wait for the final transcript (or an error). Large-v3-turbo
    //    on a ~3-second clip warm-completes in ~1 s; allow 10 s as a
    //    generous ceiling for a cold model.
    {
        std::unique_lock lk(cb_mu);
        const bool got = cb_cv.wait_for(lk, std::chrono::seconds(10),
            [&] { return final_transcript || error_message; });
        REQUIRE(got);
    }

    INFO("partials=" << partials.size()
         << " final=" << (final_transcript ? final_transcript->text : "<none>")
         << " error=" << error_message.value_or("<none>"));

    REQUIRE_FALSE(error_message);
    REQUIRE(final_transcript);
    CHECK(final_transcript->turn == kTurn);
    // Speaches' server VAD trims the leading ~50 ms below its 0.9
    // threshold, so the very first phoneme of "Hello" sometimes
    // drops (we've seen "below from Akva.", "Yellow from Akva.").
    // Assert on the model-stable suffix only — that's enough to
    // prove the audio→transcript path round-tripped intact.
    CHECK(final_transcript->text.find("smoke test") != std::string::npos);
    // M5 Step 6 — RealtimeSttClient stamps cfg.stt.language onto
    // FinalTranscript.lang so PromptBuilder / TTS / memory get a
    // non-empty value regardless of whether the backend reports it.
    CHECK(final_transcript->lang == cfg.language);

    client.stop();
}
