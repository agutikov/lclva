#include "demos/demo.hpp"

#include <array>
#include <cstdio>
#include <span>

namespace acva::demos {

// Forward declarations of each demo's entry point. Definitions live in
// the per-demo TUs so adding a new demo is one entry in this table +
// one new file under src/demos/.
int run_aec       (const config::Config&, std::span<const std::string>);
int run_aec_hw    (const config::Config&, std::span<const std::string>);
int run_aec_record(const config::Config&, std::span<const std::string>);
int run_bargein   (const config::Config&, std::span<const std::string>);
int run_capture   (const config::Config&, std::span<const std::string>);
int run_chat      (const config::Config&, std::span<const std::string>);
int run_soak      (const config::Config&, std::span<const std::string>);
int run_wedge     (const config::Config&, std::span<const std::string>);
int run_fsm       (const config::Config&, std::span<const std::string>);
int run_health    (const config::Config&, std::span<const std::string>);
int run_llm       (const config::Config&, std::span<const std::string>);
int run_loopback  (const config::Config&, std::span<const std::string>);
int run_stt       (const config::Config&, std::span<const std::string>);
int run_tone      (const config::Config&, std::span<const std::string>);
int run_transcribe(const config::Config&, std::span<const std::string>);
int run_tts       (const config::Config&, std::span<const std::string>);

namespace {

constexpr std::array<Demo, 16> kDemos{{
    {"fsm",      "M0",
     "synthetic FSM driver runs 3 turns end-to-end (no backends needed)",
     run_fsm},
    {"health",   "M2",
     "probe every configured backend's /health and print the result",
     run_health},
    {"llm",      "M1",
     "send a fixed prompt to llama and stream the reply to stdout",
     run_llm},
    {"tone",     "M3",
     "play a 1.5 s 440 Hz tone through the audio device — no TTS",
     run_tone},
    {"tts",      "M3+M4B",
     "synthesize 'Hello from acva.' via Speaches and play it",
     run_tts},
    {"chat",     "M1+M3+M4B",
     "full text-in → speech-out loop (text → LLM → Speaches TTS → speakers)",
     run_chat},
    {"bargein",  "M7",
     "auto-injected interrupt mid-turn — verifies the cancellation cascade",
     run_bargein},
    {"loopback", "M4",
     "5 s mic → speakers passthrough through the SPSC ring + 48↔16 kHz resampler",
     run_loopback},
    {"capture",  "M4",
     "5 s mic capture + Silero VAD endpointing report (no STT)",
     run_capture},
    {"stt",      "M4B",
     "self-contained STT smoke: TTS-fixture audio → Speaches /v1/audio/transcriptions",
     run_stt},
    {"transcribe", "M5",
     "30 s mic + Silero VAD + Speaches realtime STT — prints partial + final transcripts",
     run_transcribe},
    {"aec",      "M6",
     "synthetic AEC validation: stimulus → loopback → APM → ERLE convergence",
     run_aec},
    {"aec-hw",   "M6",
     "real-hardware AEC validation: speaker → mic → APM, asserts ERLE >= 25 dB",
     run_aec_hw},
    {"aec-record", "M6B",
     "record original/raw/cleaned WAVs of speaker→air→mic→APM for offline analysis",
     run_aec_record},
    {"soak",     "M6",
     "5-min Speaches TTS+STT throughput + VRAM monitor (silent; for leak detection)",
     run_soak},
    {"wedge",    "M6",
     "reproduce the Speaches CUDA-OOM wedge (long Russian TTS) and diagnose",
     run_wedge},
}};

} // namespace

std::span<const Demo> all() {
    return {kDemos.data(), kDemos.size()};
}

const Demo* find(std::string_view name) {
    for (const auto& d : kDemos) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

void print_list() {
    std::fputs(
        "Available demos. Run with `acva demo <name>`:\n\n",
        stdout);
    // Compute column width for `name`.
    std::size_t name_w = 4;
    for (const auto& d : kDemos) {
        name_w = std::max(name_w, d.name.size());
    }
    for (const auto& d : kDemos) {
        std::fprintf(stdout, "  %-*.*s  %-3.*s  %.*s\n",
                      static_cast<int>(name_w),
                      static_cast<int>(d.name.size()),         d.name.data(),
                      static_cast<int>(d.milestone.size()),    d.milestone.data(),
                      static_cast<int>(d.description.size()),  d.description.data());
    }
    std::fputs(
        "\nDemos use the same config as `acva` itself — pass --config to override.\n",
        stdout);
}

} // namespace acva::demos
