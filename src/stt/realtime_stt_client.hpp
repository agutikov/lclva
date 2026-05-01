#pragma once

#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

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

    // ---- Per-utterance API (M5 Step 2.b) ----
    //
    // The orchestrator calls these between an M4 `SpeechStarted` and
    // `SpeechEnded`. Multi-second silences within an utterance may
    // trigger Speaches' default server-side VAD to auto-commit — see
    // `open_questions.md` L5; the client tolerates extra
    // `input_audio_buffer.committed` events by tracking only the most
    // recent item id.
    struct UtteranceCallbacks {
        // Fires repeatedly as transcription deltas arrive. `text` is
        // the running concatenation of all deltas observed so far —
        // a stable prefix in OpenAI Realtime semantics; the full
        // utterance only arrives once on `on_final`.
        std::function<void(event::PartialTranscript)> on_partial;
        // Fires exactly once after `end_utterance()` (or sooner if
        // server VAD commits early). `text` is the final transcript.
        std::function<void(event::FinalTranscript)>   on_final;
        // Fires on data-channel-side errors (server `error` event,
        // unparseable response, transcription_failed). Mutually
        // exclusive with `on_final` for the same turn.
        std::function<void(std::string err)>          on_error;
    };

    // Open a new utterance scoped to `turn`. Sends
    // `input_audio_buffer.clear` to the server to discard any
    // residual audio from a prior turn. Idempotent: calling
    // `begin_utterance` again replaces the active callbacks before
    // any partial/final fires for the new id.
    void begin_utterance(dialogue::TurnId turn,
                         std::shared_ptr<dialogue::CancellationToken> cancel,
                         UtteranceCallbacks cb);

    // Push a chunk of 16 kHz mono int16 PCM. Resampled to 24 kHz
    // mono internally and sent as `input_audio_buffer.append`. Safe
    // to call from the M4 audio-pipeline worker thread; the data
    // channel send is non-blocking (libdatachannel's SCTP buffer
    // owns backpressure).
    void push_audio(std::span<const std::int16_t> samples_16k);

    // Signal end-of-utterance. Flushes the resampler tail, sends
    // `input_audio_buffer.commit`, and returns immediately. The
    // matching `on_final` arrives asynchronously when the server
    // finishes transcribing.
    void end_utterance();

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
