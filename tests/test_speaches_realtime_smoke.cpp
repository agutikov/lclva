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
