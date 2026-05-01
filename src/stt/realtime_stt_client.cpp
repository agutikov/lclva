#include "stt/realtime_stt_client.hpp"

#include "log/log.hpp"

#ifdef ACVA_HAVE_LIBDATACHANNEL
#include "stt/realtime_envelope.hpp"

#include <glaze/glaze.hpp>
#include <httplib.h>
#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <utility>
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
// We deliberately do NOT touch `turn_detection` here. Speaches'
// `PartialSession` schema (src/speaches/types/realtime.py) types it as
// `TurnDetection | NotGiven` — null is rejected and there is no
// "disable" variant. Server-side VAD therefore stays on with its
// default thresholds. Our M4 Silero pipeline still owns the bus-level
// `SpeechStarted`/`SpeechEnded` events and the orchestrator's
// dialogue FSM ignores Speaches' `input_audio_buffer.speech_*` echoes.
// (Open question for M5 — see plans/open_questions.md section L.)
std::string build_session_update_json(const std::string& model_id) {
    std::string s;
    s.reserve(256);
    s += R"({"event_id":")";
    s += random_event_id();
    s += R"(","type":"session.update","session":{)";
    s += R"("modalities":["text"],)";
    s += R"("tools":[],)";
    s += R"("input_audio_transcription":{"model":")";
    s += model_id;
    s += R"("}}})";
    return s;
}

struct InnerEvent {
    std::string                type;
    std::optional<std::string> error_message;
};

struct TypeOnly {
    std::string type;
};
struct ErrorPayload {
    std::optional<std::string> message;
};
struct ErrorEnvelope {
    ErrorPayload error;
};

constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};

InnerEvent classify_inner_event(std::string_view inner_json) {
    InnerEvent ev;
    TypeOnly t;
    if (auto ec = glz::read<kReadOpts>(t, inner_json); !ec) {
        ev.type = std::move(t.type);
    }
    if (ev.type == "error") {
        ErrorEnvelope env;
        if (auto ec = glz::read<kReadOpts>(env, inner_json); !ec) {
            ev.error_message = std::move(env.error.message);
        }
    }
    return ev;
}

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

    void set_state(State s) {
        // Caller must hold `mu`.
        state = s;
        cv.notify_all();
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
        dc->onMessage([this](rtc::message_variant data) {
            if (!std::holds_alternative<std::string>(data)) return;
            const auto& raw = std::get<std::string>(data);
            auto inner = reassembler.feed(raw);
            if (!inner) return;

            auto ev = classify_inner_event(*inner);
            std::lock_guard lk(mu);
            if (ev.type == "session.updated" && state == State::Configuring) {
                set_state(State::Ready);
            } else if (ev.type == "error") {
                log::warn("stt-realtime",
                          std::string{"server error: "}
                              + ev.error_message.value_or("<no message>"));
                if (state == State::Configuring || state == State::Connecting) {
                    set_state(State::Failed);
                }
            }
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
    const std::string update = build_session_update_json(cfg_.model);
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
        pc = impl_->pc;
    }
    if (pc) pc->close();
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

#endif

} // namespace acva::stt
