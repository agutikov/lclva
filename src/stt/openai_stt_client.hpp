#pragma once

#include "audio/utterance.hpp"
#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <functional>
#include <memory>
#include <string>

namespace acva::stt {

// One transcription request — an utterance produced by the M4 audio
// pipeline.
struct SttRequest {
    dialogue::TurnId                       turn   = event::kNoTurn;
    std::shared_ptr<audio::AudioSlice>     slice;
    std::shared_ptr<dialogue::CancellationToken> cancel;
    // Optional language hint (BCP-47). Empty → let Whisper detect.
    std::string                            lang_hint;
};

struct SttCallbacks {
    // Called once on successful transcription.
    std::function<void(event::FinalTranscript)> on_final;
    // Called once on any failure (network, non-2xx, JSON parse,
    // cancellation). Mutually exclusive with on_final.
    std::function<void(std::string err)>        on_error;
};

// OpenAI-API-compatible STT client.
//
// POSTs the AudioSlice as a multipart file (WAV-wrapped 16 kHz mono
// int16) to `{cfg.stt.base_url}/audio/transcriptions` and parses
// Speaches' JSON response.
//
// **Synchronous.** submit() blocks until the response arrives or the
// cancellation token flips. main.cpp runs it on a dedicated I/O thread
// (one in flight at a time during M4B; M5 will swap this for
// streaming + speculation against /v1/realtime).
//
// **Streaming partials are out of scope here** — that's M5.
class OpenAiSttClient {
public:
    explicit OpenAiSttClient(const config::SttConfig& cfg);
    ~OpenAiSttClient();

    OpenAiSttClient(const OpenAiSttClient&)            = delete;
    OpenAiSttClient& operator=(const OpenAiSttClient&) = delete;
    OpenAiSttClient(OpenAiSttClient&&)                 = delete;
    OpenAiSttClient& operator=(OpenAiSttClient&&)      = delete;

    void submit(SttRequest req, SttCallbacks cb);

    // HEAD against /health for startup probes.
    [[nodiscard]] bool probe();

private:
    const config::SttConfig& cfg_;
};

// Force a Whisper model load by submitting a 1 s silent transcription.
// Blocks until Speaches returns (success or HTTP error). Used by
// main.cpp at pipeline open and by `acva demo soak` to absorb the
// cold-load latency before any throughput measurement starts.
struct WarmupResult {
    bool        ok = false;
    long long   ms = 0;
    std::string error;        // empty on success
};
[[nodiscard]] WarmupResult warmup(const config::SttConfig& cfg);

} // namespace acva::stt
