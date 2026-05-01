#pragma once

#include "config/config.hpp"
#include "tts/types.hpp"

#include <string_view>

namespace acva::tts {

// OpenAI-API-compatible TTS client. Talks `POST /v1/audio/speech`
// against a single Speaches base URL (cfg.tts.base_url) — voice
// selection is per-language via cfg.tts.voices[lang].model_id (and
// optional voice_id) instead of M3's per-language URL.
//
// Drop-in for PiperClient: same TtsRequest / TtsCallbacks types, same
// thread contract (synchronous, called from the TtsBridge I/O thread).
//
// **Streaming.** Uses libcurl's WRITEFUNCTION callback to forward
// response bytes to TtsCallbacks::on_audio as they arrive — TTFB on
// the dev workstation is ~10 ms vs PiperClient's ~600 ms full-buffer.
// Realising the win in TtsBridge requires the bridge's enqueue path to
// not buffer the whole sentence either; see m4b_speaches_consolidation.md
// Step 4.
//
// **Format.** Always uses `response_format=pcm` — bare int16 mono
// 22050 Hz. Avoids Speaches' streaming-broken WAV header (the
// `data_size` field reports the first chunk only, so a naive WAV
// parser would truncate the audio to ~1.4 s; PCM dodges the issue).
class OpenAiTtsClient {
public:
    explicit OpenAiTtsClient(const config::TtsConfig& cfg);
    ~OpenAiTtsClient();

    OpenAiTtsClient(const OpenAiTtsClient&)            = delete;
    OpenAiTtsClient& operator=(const OpenAiTtsClient&) = delete;
    OpenAiTtsClient(OpenAiTtsClient&&)                 = delete;
    OpenAiTtsClient& operator=(OpenAiTtsClient&&)      = delete;

    // Submit one synthesis request. Blocks until the full PCM stream
    // has been delivered to on_audio or the cancellation token flips.
    // Reentrancy: not safe to call concurrently from multiple threads
    // on the same instance — schedule serial submissions on the
    // bridge's I/O thread (mirrors PiperClient).
    void submit(TtsRequest req, TtsCallbacks cb);

    // Resolve a language to its model id (and optional voice id).
    // Returns empty model_id when neither the requested language nor
    // the fallback is configured.
    struct VoiceRoute {
        std::string model_id;
        std::string voice_id;
    };
    [[nodiscard]] VoiceRoute route_for(std::string_view lang) const;

    // Issue a HEAD against the base URL's /health to verify
    // reachability. Returns true on any 2xx response (Speaches answers
    // 200), false on network failure.
    [[nodiscard]] bool probe();

private:
    const config::TtsConfig& cfg_;
};

} // namespace acva::tts
