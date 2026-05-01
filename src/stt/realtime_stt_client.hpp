#pragma once

#include "config/config.hpp"

#include <chrono>
#include <memory>

namespace acva::stt {

// Streaming STT client for Speaches' WebRTC realtime endpoint.
//
// Owns one long-lived `rtc::PeerConnection` against
// `POST /v1/realtime?model=<id>` (see `project_m5_realtime_spike.md` for
// resolved wire-protocol facts). The session lifecycle is:
//
//   Idle → Connecting → Configuring → Ready
//              ↓             ↓
//            Failed        Failed
//
// `start()` blocks until Ready or the timeout expires. Once Ready, the
// data channel is open, the session has been configured (transcription
// model pinned, server-side VAD disabled, output modalities reduced to
// text-only), and the client is prepared for per-utterance audio +
// transcript wiring (M5 Step 2.b).
//
// **STT-only.** The session.update sent on bring-up disables Speaches'
// default Kokoro TTS path (`modalities: ["text"]`) and server-side VAD
// (`turn_detection: null`); utterance boundaries are owned by the
// orchestrator's M4 Silero pipeline.
//
// **Compile-time gated** on `ACVA_HAVE_LIBDATACHANNEL`. When the
// dependency is absent, `start()` returns false immediately and the
// client stays in `Idle`. The header is unconditional so call sites
// don't need to branch.
class RealtimeSttClient {
public:
    enum class State {
        Idle,         // before start(), or after stop()
        Connecting,   // ICE+SDP+DTLS handshake in flight
        Configuring,  // session.update sent, awaiting session.updated
        Ready,        // session.updated received and accepted
        Failed,       // any unrecoverable error during bring-up
        Closed,       // stop() called after a successful start
    };

    explicit RealtimeSttClient(const config::SttConfig& cfg);
    ~RealtimeSttClient();

    RealtimeSttClient(const RealtimeSttClient&)            = delete;
    RealtimeSttClient& operator=(const RealtimeSttClient&) = delete;
    RealtimeSttClient(RealtimeSttClient&&)                 = delete;
    RealtimeSttClient& operator=(RealtimeSttClient&&)      = delete;

    // Bring up the long-lived realtime session. Synchronous: blocks
    // until State::Ready (returns true) or State::Failed / timeout
    // (returns false). Safe to call exactly once per instance; a
    // second call after stop() requires a fresh client.
    [[nodiscard]] bool start(
        std::chrono::milliseconds timeout = std::chrono::seconds(15));

    // Tear down the peer connection. Idempotent; safe to call from
    // any thread including from inside the client's own callbacks.
    void stop();

    [[nodiscard]] State state() const;

    // HEAD against the configured `/health` for startup probes —
    // shares the path with OpenAiSttClient so the supervisor's
    // service monitor doesn't need a separate code path.
    [[nodiscard]] bool probe();

private:
    struct Impl;
    std::unique_ptr<Impl>  impl_;
    const config::SttConfig& cfg_;
};

} // namespace acva::stt
