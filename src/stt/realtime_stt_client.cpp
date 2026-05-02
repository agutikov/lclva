#include "stt/realtime_stt_client.hpp"

#include "log/log.hpp"

#ifdef ACVA_HAVE_LIBDATACHANNEL
#include "audio/resampler.hpp"
#include "stt/realtime_envelope.hpp"
#include "stt/realtime_event_dispatch.hpp"

#include <glaze/glaze.hpp>
#include <httplib.h>
#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>
#endif

namespace acva::stt {

#ifdef ACVA_HAVE_LIBDATACHANNEL

namespace {

struct ParsedUrl {
    std::string host;
    int         port = 0;
    std::string path_prefix; // typically "/v1"
    bool        ok   = false;
};

ParsedUrl parse_base_url(const std::string& url) {
    ParsedUrl r;
    static const std::string kPrefix = "http://";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) return r;
    auto rest      = url.substr(kPrefix.size());
    auto slash     = rest.find('/');
    auto authority = rest.substr(0, slash == std::string::npos ? rest.size() : slash);
    r.path_prefix  = slash == std::string::npos ? std::string{} : rest.substr(slash);
    auto colon     = authority.find(':');
    r.host = authority.substr(0, colon);
    r.port = colon == std::string::npos ? 80
              : std::atoi(authority.substr(colon + 1).c_str());
    r.ok   = !r.host.empty() && r.port > 0;
    return r;
}

std::string random_event_id() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    static constexpr std::string_view alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string s = "evt_";
    for (int i = 0; i < 20; ++i) {
        s.push_back(alpha[gen() % alpha.size()]);
    }
    return s;
}

// session.update payload pinning Speaches to STT-only mode with our
// chosen transcription model.
//
// `turn_detection.create_response: false` is the load-bearing flag.
// With it true (the Speaches default), the realtime endpoint
// auto-runs `generate_response` after every transcription —
// Speaches' chat-completion path against its own loopback, which it
// doesn't host, returns 500, and the failure cascades into a
// duplicate-conversation-item error. We don't want that path at all
// — our Manager owns LLM generation against llama directly. So we
// send a fully-specified TurnDetection (the schema requires every
// field), changing only create_response.
//
// `turn_detection: null` is NOT accepted by Speaches'
// `PartialSession` schema; see open_questions.md L5.
std::string build_session_update_json(const std::string& model_id,
                                       const std::string& language) {
    std::string s;
    s.reserve(384);
    s += R"({"event_id":")";
    s += random_event_id();
    s += R"(","type":"session.update","session":{)";
    s += R"("modalities":["text"],)";
    s += R"("tools":[],)";
    // Speaches' TurnDetection schema requires prefix_padding_ms (no
    // default). Omitting it silently fails the union-validation
    // against `TurnDetection | NotGiven` — Pydantic falls through to
    // `NotGiven` and drops the entire turn_detection block from the
    // update, including our `create_response: false`. So we MUST
    // send all required fields. The session.update handler then
    // broadcasts an `invalid_request_error` event for
    // prefix_padding_ms specifically (it's marked unsupported via
    // the dump's exclude argument); my dispatcher tolerates that
    // error during bring-up. The session itself applies cleanly
    // with create_response: false in effect — which is the actual
    // goal: stop Speaches from auto-running chat-completion against
    // its own (unhosted) /v1/chat/completions after every transcript.
    // Effectively disable Speaches' server VAD: with threshold=1.0
    // and Silero's confidence range [0,1], speech is never detected
    // → audio_start_ms stays None → no auto speech_stopped, no
    // auto-commit. Our M4 Silero owns utterance boundaries; we
    // send `input_audio_buffer.commit` ourselves in end_utterance().
    //
    // Why we can't just send `turn_detection: null` (the obvious
    // fix): Speaches' `PartialSession` schema rejects null. Why we
    // can't keep server VAD on with sane thresholds: with the
    // server-locked `prefix_padding_ms: 0`, server VAD fires
    // rapid-fire speech_started/stopped pairs during real speech
    // (at ~10 ms intervals) and the resulting auto-commit storm
    // produces duplicate-item errors. See open_questions.md L5.
    s += R"("turn_detection":{)";
    s +=     R"("type":"server_vad",)";
    s +=     R"("create_response":false,)";
    s +=     R"("prefix_padding_ms":0,)";
    s +=     R"("silence_duration_ms":550,)";
    s +=     R"("threshold":1.0)";
    s += R"(},)";
    s += R"("input_audio_transcription":{"model":")";
    s += model_id;
    if (!language.empty()) {
        s += R"(","language":")";
        s += language;
    }
    s += R"("}}})";
    return s;
}

// PCM resample target — Speaches' realtime endpoint normalizes all
// inbound audio to 24 kHz mono int16 internally (see
// `routers/realtime/rtc.py` SAMPLE_RATE constant), so we resample
// once on the way out instead of paying the server-side resample
// cost on every chunk.
constexpr double  kInputSampleRate  = 16'000.0;
constexpr double  kSpeachesPcmRate  = 24'000.0;

} // namespace

struct RealtimeSttClient::Impl {
    explicit Impl(const config::SttConfig& cfg_) : cfg(cfg_) {}

    const config::SttConfig& cfg;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel>    dc;
    std::shared_ptr<rtc::Track>          audio;
    realtime::EnvelopeReassembler        reassembler;

    mutable std::mutex                  mu;
    std::condition_variable             cv;
    State                               state = State::Idle;
    bool                                ice_gathered = false;
    bool                                channel_open = false;
    rtc::PeerConnection::State          pc_state = rtc::PeerConnection::State::New;

    // ---- per-utterance state ----
    // All fields below are guarded by `mu`. `resampler` is only
    // accessed under the mutex; soxr's process()/flush() are not
    // thread-safe and the M4 audio worker only owns `push_audio`,
    // not the libdatachannel callbacks that mutate state.
    dialogue::TurnId                                  active_turn = event::kNoTurn;
    std::shared_ptr<dialogue::CancellationToken>      active_cancel;
    UtteranceCallbacks                                active_cb;
    bool                                              active = false;
    std::string                                       active_item_id;       // server-assigned, set on input_audio_buffer.committed
    std::string                                       partial_text;         // running concatenation of deltas
    event::SequenceNo                                 partial_seq = 0;
    std::chrono::steady_clock::time_point             utterance_start{};
    std::unique_ptr<audio::Resampler>                 resampler;

    void set_state(State s) {
        // Caller must hold `mu`.
        state = s;
        cv.notify_all();
    }

    realtime::EventCallbacks dispatcher_callbacks() {
        // All user-facing callbacks run *outside* `mu` to avoid
        // deadlock if they re-enter the client (state(), push_audio,
        // end_utterance). libdatachannel serializes onMessage per
        // channel, so dropping the lock between dispatch invocations
        // does not introduce concurrent reentry.
        realtime::EventCallbacks cb;
        cb.on_session_updated = [this] {
            std::lock_guard lk(mu);
            if (state == State::Configuring) set_state(State::Ready);
        };
        cb.on_committed = [this](std::string item_id) {
            std::lock_guard lk(mu);
            if (active) active_item_id = std::move(item_id);
        };
        cb.on_partial = [this](std::string item_id, std::string delta) {
            std::function<void(event::PartialTranscript)> user_cb;
            event::PartialTranscript ev;
            {
                std::lock_guard lk(mu);
                if (!active || item_id != active_item_id) return;
                if (active_cancel && active_cancel->is_cancelled()) return;
                partial_text += delta;
                ev.turn              = active_turn;
                ev.text              = partial_text;
                ev.stable_prefix_len = partial_text.size();
                ev.seq               = ++partial_seq;
                user_cb = active_cb.on_partial;
            }
            if (user_cb) user_cb(std::move(ev));
        };
        cb.on_final = [this](std::string item_id, std::string transcript,
                              std::string language) {
            std::function<void(event::FinalTranscript)> user_cb;
            event::FinalTranscript ev;
            {
                std::lock_guard lk(mu);
                if (!active || item_id != active_item_id) return;
                if (active_cancel && active_cancel->is_cancelled()) {
                    clear_active_locked();
                    return;
                }
                ev.turn = active_turn;
                ev.text = std::move(transcript);
                // Speaches' transcription.completed event has no
                // `language` field (the OpenAI Realtime spec it
                // implements doesn't carry it). Stamp the configured
                // language from cfg.stt.language so PromptBuilder /
                // TTS / memory get a non-empty value. M9 will replace
                // this with per-utterance detection once a streaming
                // backend that emits language is in place.
                ev.lang = !language.empty() ? std::move(language) : cfg.language;
                ev.processing_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - utterance_start);
                user_cb = std::move(active_cb.on_final);
                clear_active_locked();
            }
            if (user_cb) user_cb(std::move(ev));
        };
        cb.on_server_error = [this](std::string message) {
            // Server-side error events are informational — Speaches
            // broadcasts them for things like "unsupported field"
            // alongside an actually-applied session.update. We don't
            // fail the bring-up here; if the session.updated event
            // never arrives, start()'s deadline catches it. Errors
            // during an active utterance fail just that turn (the
            // session itself stays alive — server typically recovers).
            log::warn("stt-realtime", std::string{"server error: "} + message);
            std::function<void(std::string)> user_cb;
            std::string err_copy;
            {
                std::lock_guard lk(mu);
                if (active) {
                    user_cb = std::move(active_cb.on_error);
                    err_copy = message;
                    clear_active_locked();
                }
            }
            if (user_cb) user_cb(std::move(err_copy));
        };
        return cb;
    }

    // Caller must hold `mu`.
    void clear_active_locked() {
        active = false;
        active_turn = event::kNoTurn;
        active_item_id.clear();
        partial_text.clear();
        partial_seq = 0;
        active_cb = {};
        active_cancel.reset();
        resampler.reset();
    }

    void wire_callbacks() {
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState g) {
            if (g != rtc::PeerConnection::GatheringState::Complete) return;
            std::lock_guard lk(mu);
            ice_gathered = true;
            cv.notify_all();
        });
        pc->onStateChange([this](rtc::PeerConnection::State s) {
            std::lock_guard lk(mu);
            pc_state = s;
            if (s == rtc::PeerConnection::State::Failed
             || s == rtc::PeerConnection::State::Closed
             || s == rtc::PeerConnection::State::Disconnected) {
                if (state != State::Ready && state != State::Closed) {
                    set_state(State::Failed);
                }
            }
            cv.notify_all();
        });
        dc->onOpen([this] {
            std::lock_guard lk(mu);
            channel_open = true;
            cv.notify_all();
        });
        const auto cb = dispatcher_callbacks();
        dc->onMessage([this, cb](rtc::message_variant data) {
            if (!std::holds_alternative<std::string>(data)) return;
            const auto& raw = std::get<std::string>(data);
            auto inner = reassembler.feed(raw);
            if (!inner) return;
            realtime::dispatch(*inner, cb);
        });
    }

    bool wait_until(std::chrono::steady_clock::time_point deadline,
                    auto&& predicate) {
        std::unique_lock lk(mu);
        return cv.wait_until(lk, deadline, predicate);
    }
};

RealtimeSttClient::RealtimeSttClient(const config::SttConfig& cfg)
    : impl_(std::make_unique<Impl>(cfg)), cfg_(cfg) {}

RealtimeSttClient::~RealtimeSttClient() { stop(); }

RealtimeSttClient::State RealtimeSttClient::state() const {
    std::lock_guard lk(impl_->mu);
    return impl_->state;
}

bool RealtimeSttClient::probe() {
    auto p = parse_base_url(cfg_.base_url);
    if (!p.ok) return false;
    httplib::Client c(p.host, p.port);
    c.set_connection_timeout(std::chrono::seconds(2));
    auto r = c.Get("/health");
    return r && r->status == 200;
}

bool RealtimeSttClient::start(std::chrono::milliseconds timeout) {
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state != State::Idle) return false;
        impl_->state = State::Connecting;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    rtc::Configuration rtc_cfg;
    rtc_cfg.iceTransportPolicy = rtc::TransportPolicy::All;
    impl_->pc = std::make_shared<rtc::PeerConnection>(rtc_cfg);

    rtc::Description::Audio audio_desc(
        "audio", rtc::Description::Direction::SendOnly);
    audio_desc.addOpusCodec(111);
    impl_->audio = impl_->pc->addTrack(audio_desc);

    impl_->dc = impl_->pc->createDataChannel("oai-events");
    impl_->wire_callbacks();
    impl_->pc->setLocalDescription();

    // 1. Wait for ICE gathering.
    if (!impl_->wait_until(deadline,
            [this] { return impl_->ice_gathered || impl_->state == State::Failed; })) {
        log::warn("stt-realtime", "timeout waiting for ICE gathering");
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state == State::Failed) return false;
    }

    // 2. POST the offer.
    auto local = impl_->pc->localDescription();
    if (!local) {
        log::warn("stt-realtime", "no local description after ICE gather");
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    std::string offer_sdp = std::string(*local);

    auto p = parse_base_url(cfg_.base_url);
    if (!p.ok) {
        log::warn("stt-realtime",
                  std::string{"unparseable base_url: "} + cfg_.base_url);
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    httplib::Client http(p.host, p.port);
    http.set_connection_timeout(std::chrono::seconds(5));
    http.set_read_timeout(std::chrono::seconds(15));
    const std::string path = p.path_prefix + "/realtime?model=" + cfg_.model;

    auto res = http.Post(path.c_str(), offer_sdp, "application/sdp");
    if (!res || res->status != 200 || res->body.find("v=0") != 0) {
        log::warn("stt-realtime",
                  std::string{"POST /realtime failed: status="}
                      + (res ? std::to_string(res->status) : std::string{"<no res>"}));
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }

    // 3. Apply answer; wait for ICE+DTLS connected.
    impl_->pc->setRemoteDescription(rtc::Description(res->body, "answer"));

    if (!impl_->wait_until(deadline, [this] {
        return impl_->pc_state == rtc::PeerConnection::State::Connected
            || impl_->state == State::Failed;
    })) {
        log::warn("stt-realtime", "timeout waiting for peer connected");
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state == State::Failed) return false;
    }

    // 4. Wait for the data channel to open.
    if (!impl_->wait_until(deadline, [this] {
        return impl_->channel_open || impl_->state == State::Failed;
    })) {
        log::warn("stt-realtime", "timeout waiting for data channel open");
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state == State::Failed) return false;
    }

    // 5. Send session.update — pin transcription model, disable
    //    Kokoro TTS, disable server-side VAD (M4 Silero owns
    //    utterance boundaries).
    {
        std::lock_guard lk(impl_->mu);
        impl_->state = State::Configuring;
    }
    const std::string update = build_session_update_json(cfg_.model, cfg_.language);
    impl_->dc->send(update);

    // 6. Wait for session.updated.
    if (!impl_->wait_until(deadline, [this] {
        return impl_->state == State::Ready || impl_->state == State::Failed;
    })) {
        log::warn("stt-realtime", "timeout waiting for session.updated");
        std::lock_guard lk(impl_->mu);
        impl_->set_state(State::Failed);
        return false;
    }
    return state() == State::Ready;
}

void RealtimeSttClient::stop() {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state == State::Idle || impl_->state == State::Closed) return;
        impl_->state = State::Closed;
        impl_->clear_active_locked();
        pc = impl_->pc;
    }
    if (pc) pc->close();
}

void RealtimeSttClient::begin_utterance(
        dialogue::TurnId turn,
        std::shared_ptr<dialogue::CancellationToken> cancel,
        UtteranceCallbacks cb) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard lk(impl_->mu);
        if (impl_->state != State::Ready) {
            if (cb.on_error) {
                std::function<void(std::string)> err = std::move(cb.on_error);
                // Defer the user error callback outside the lock.
                err("realtime stt session not Ready");
            }
            return;
        }
        // Replace any in-flight utterance state. begin_utterance is
        // idempotent at the caller's discretion — barge-in races may
        // call it back-to-back.
        impl_->clear_active_locked();
        impl_->active            = true;
        impl_->active_turn       = turn;
        impl_->active_cancel     = std::move(cancel);
        impl_->active_cb         = std::move(cb);
        impl_->utterance_start   = std::chrono::steady_clock::now();
        impl_->resampler         = std::make_unique<audio::Resampler>(
            kInputSampleRate, kSpeachesPcmRate, audio::Resampler::Quality::High);
        dc = impl_->dc;
    }
    if (!dc) return;
    // Discard any residual server-side audio from a prior utterance.
    dc->send(realtime::build_simple_event_json(
        random_event_id(), "input_audio_buffer.clear"));
}

void RealtimeSttClient::push_audio(std::span<const std::int16_t> samples_16k) {
    std::shared_ptr<rtc::DataChannel> dc;
    std::vector<std::int16_t>         resampled;
    {
        std::lock_guard lk(impl_->mu);
        if (!impl_->active || !impl_->resampler) return;
        if (impl_->active_cancel && impl_->active_cancel->is_cancelled()) return;
        resampled = impl_->resampler->process(samples_16k);
        dc = impl_->dc;
    }
    if (resampled.empty() || !dc) return;
    const auto* bytes = reinterpret_cast<const char*>(resampled.data());
    const auto  nbyte = resampled.size() * sizeof(std::int16_t);
    // Debug dump: when ACVA_RT_DEBUG_DUMP=path is set, append the raw
    // 24 kHz PCM going on the wire to that file so we can compare it
    // against Speaches' /v1/audio/transcriptions.
    if (const char* dump = std::getenv("ACVA_RT_DEBUG_DUMP"); dump && *dump) {
        if (FILE* f = std::fopen(dump, "ab")) {
            std::fwrite(bytes, 1, nbyte, f);
            std::fclose(f);
        }
    }
    const std::string b64 = realtime::base64_encode({bytes, nbyte});
    dc->send(realtime::build_input_audio_buffer_append_json(
        random_event_id(), b64));
}

void RealtimeSttClient::end_utterance() {
    std::shared_ptr<rtc::DataChannel> dc;
    std::vector<std::int16_t>         tail;
    {
        std::lock_guard lk(impl_->mu);
        if (!impl_->active || !impl_->resampler) return;
        tail = impl_->resampler->flush();
        dc = impl_->dc;
        impl_->resampler.reset();
    }
    if (!dc) return;
    if (!tail.empty()) {
        const auto* bytes = reinterpret_cast<const char*>(tail.data());
        const auto  nbyte = tail.size() * sizeof(std::int16_t);
        const std::string b64 = realtime::base64_encode({bytes, nbyte});
        dc->send(realtime::build_input_audio_buffer_append_json(
            random_event_id(), b64));
    }
    // Server VAD is parked at threshold=1.0 (effectively off — see
    // build_session_update_json) so it never auto-commits. We commit
    // explicitly here on each M4 SpeechEnded; the server publishes
    // `input_audio_buffer.committed` and runs transcription on the
    // buffer we've been appending to.
    dc->send(realtime::build_simple_event_json(
        random_event_id(), "input_audio_buffer.commit"));
}

#else // !ACVA_HAVE_LIBDATACHANNEL

struct RealtimeSttClient::Impl {};

RealtimeSttClient::RealtimeSttClient(const config::SttConfig& cfg) : cfg_(cfg) {}
RealtimeSttClient::~RealtimeSttClient() = default;

bool RealtimeSttClient::start(std::chrono::milliseconds /*timeout*/) {
    log::warn("stt-realtime",
              "RealtimeSttClient::start called but libdatachannel unavailable");
    return false;
}
void RealtimeSttClient::stop() {}
RealtimeSttClient::State RealtimeSttClient::state() const {
    return State::Idle;
}
bool RealtimeSttClient::probe() { return false; }

void RealtimeSttClient::begin_utterance(
        dialogue::TurnId /*turn*/,
        std::shared_ptr<dialogue::CancellationToken> /*cancel*/,
        UtteranceCallbacks cb) {
    if (cb.on_error) cb.on_error("libdatachannel unavailable");
}
void RealtimeSttClient::push_audio(std::span<const std::int16_t>) {}
void RealtimeSttClient::end_utterance() {}

#endif

} // namespace acva::stt
