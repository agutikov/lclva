#pragma once

#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace acva::tts {

// One synthesis request. The (turn, seq) pair tags downstream events
// (TtsAudioChunk, TtsFinished) and is also what the playback queue
// uses to drop stale chunks during barge-in.
struct TtsRequest {
    dialogue::TurnId turn = event::kNoTurn;
    event::SequenceNo seq = 0;
    std::string text;
    // BCP-47 language tag (e.g. "en", "ru"). Selects the voice via
    // cfg.tts.voices; if missing, the client falls back to
    // cfg.tts.fallback_lang. If that is also unconfigured, submit()
    // calls on_error and returns immediately.
    std::string lang;
    // Per-turn cancellation. Checked before issuing the request and on
    // every received chunk. When the token flips, submit() aborts
    // promptly and on_error fires with "cancelled".
    std::shared_ptr<dialogue::CancellationToken> cancel;
};

// Callbacks fire on the calling thread for the duration of submit().
// The TTS clients never spawn their own threads — schedule submit()
// onto a dedicated I/O thread (the TtsBridge owns this thread).
struct TtsCallbacks {
    // Called once when the format is known. For Piper-backed Speaches
    // voices the rate is 22050 Hz; the bridge's resampler reads this
    // to set its in-rate.
    std::function<void(int sample_rate_hz)> on_format;

    // Called zero or more times with mono int16 PCM samples as they
    // stream from the backend.
    std::function<void(std::span<const std::int16_t> samples)> on_audio;

    // Called exactly once when submit() returns; mutually exclusive
    // with on_error.
    std::function<void()> on_finished;

    // Called exactly once on any error: bad URL, network failure,
    // non-2xx status, malformed payload, or cancellation. Mutually
    // exclusive with on_finished.
    std::function<void(std::string err)> on_error;
};

} // namespace acva::tts
