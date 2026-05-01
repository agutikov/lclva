#pragma once

#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace acva::tts {

// One synthesis request. The (turn, seq) pair tags downstream events
// (TtsAudioChunk, TtsFinished) and is also what the playback queue
// uses to drop stale chunks during barge-in.
struct TtsRequest {
    dialogue::TurnId turn = event::kNoTurn;
    event::SequenceNo seq = 0;
    std::string text;
    // BCP-47 language tag (e.g. "en", "ru"). Selects the voice URL via
    // cfg.tts.voices; if missing, PiperClient falls back to
    // cfg.tts.fallback_lang. If that is also unconfigured, submit()
    // calls on_error and returns immediately.
    std::string lang;
    // Per-turn cancellation. Checked before issuing the request and on
    // every received chunk. When the token flips, submit() aborts
    // promptly and on_error fires with "cancelled".
    std::shared_ptr<dialogue::CancellationToken> cancel;
};

// Callbacks fire on the calling thread for the duration of submit().
// PiperClient never spawns its own thread — schedule submit() onto a
// dedicated I/O thread (the TtsBridge owns this thread in M3.6).
struct TtsCallbacks {
    // Called once after the WAV header is parsed, before any audio.
    // The reported sample rate comes from the response — Piper voices
    // ship at 16 kHz / 22.05 kHz / 24 kHz depending on the model.
    std::function<void(int sample_rate_hz)> on_format;

    // Called zero or more times with mono int16 PCM samples.
    std::function<void(std::span<const std::int16_t> samples)> on_audio;

    // Called exactly once when submit() returns; mutually exclusive
    // with on_error.
    std::function<void()> on_finished;

    // Called exactly once on any error: bad URL, network failure,
    // non-2xx status, malformed WAV, or cancellation. Mutually
    // exclusive with on_finished.
    std::function<void(std::string err)> on_error;
};

// Synchronous HTTP client around upstream `python -m piper.http_server`.
// Per the M3 design (project_design.md §4.9), per-language voices are
// chosen by URL — the voice is implicit in the route — so the client
// only needs the (lang → URL) map from config.
class PiperClient {
public:
    explicit PiperClient(const config::TtsConfig& cfg) noexcept;

    // Submit a synthesis request. Blocks until the response is fully
    // consumed or the cancellation token flips. Threadsafe in the sense
    // that multiple PiperClients can be used concurrently from
    // different threads, but an individual submit() is not reentrant
    // on the same instance — schedule serial submissions on the
    // bridge's I/O thread.
    void submit(TtsRequest req, TtsCallbacks cb);

    // Resolve a language to a voice URL. Used by submit() and exposed
    // for tests / metrics / logging.
    //
    //   • cfg.tts.voices[lang]          if present
    //   • cfg.tts.voices[fallback_lang] otherwise
    //   • empty string                  if neither is configured
    [[nodiscard]] std::string url_for(std::string_view lang) const;

    // Issue a HEAD request to the resolved URL to verify reachability.
    // Returns true on any 2xx/4xx response (server is alive); false on
    // network failure. Used by main.cpp at startup; the supervisor's
    // /health probes are a separate path (when configured).
    [[nodiscard]] bool probe(std::string_view lang);

private:
    const config::TtsConfig& cfg_;
};

} // namespace acva::tts
