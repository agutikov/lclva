#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace acva::config {

enum class LogSink {
    stderr_,
    journal,
    file,
};

struct LoggingConfig {
    std::string level = "info"; // trace | debug | info | warn | error | critical | off
    std::string sink = "stderr"; // "stderr" | "journal" | "file"
    std::optional<std::string> file_path;
};

struct ControlConfig {
    std::string bind = "127.0.0.1";
    uint16_t port = 9876;
};

struct PipelineConfig {
    // Drive the FSM with synthetic events (no real audio/STT/LLM/TTS).
    // The default in the YAML config is true at M0 since no real backends
    // are wired yet.
    bool fake_driver_enabled = true;
    // Sentences per synthetic turn.
    uint32_t fake_sentences_per_turn = 3;
    // Idle between synthetic turns, in ms.
    uint32_t fake_idle_between_turns_ms = 1500;
    // Barge-in probability per turn (0.0 .. 1.0).
    double fake_barge_in_probability = 0.0;
};

// Per-service health probe knobs. Embedded in each backend's config
// section (llm/stt/tts) so the user can tune services independently. The
// Supervisor (M2) reads these to drive ServiceMonitor.
struct ServiceHealthConfig {
    // Empty disables this service's probe entirely (the Supervisor still
    // registers it with state=NotConfigured for /status visibility).
    std::string health_url;
    // If true and the service stays Unhealthy past the supervisor's grace
    // window, the dialogue path is gated and refuses new turns.
    bool fail_pipeline_if_down = true;
    uint32_t probe_interval_healthy_ms = 5000;
    uint32_t probe_interval_degraded_ms = 1000;
    // Consecutive probe failures from Healthy before transitioning to
    // Unhealthy. Failures within [1, this) sit in Degraded.
    uint32_t degraded_max_failures = 3;
};

struct LlmConfig {
    std::string base_url = "http://127.0.0.1:8081/v1";
    std::string model = "qwen2.5-7b-instruct";
    std::string unit = "acva-llama.service";
    double temperature = 0.7;
    uint32_t max_tokens = 400;
    uint32_t max_prompt_tokens = 3000;
    uint32_t request_timeout_seconds = 60;
    uint32_t keep_alive_interval_seconds = 60;
    ServiceHealthConfig health;
};

struct SttConfig {
    ServiceHealthConfig health;
};

struct TtsConfig {
    ServiceHealthConfig health;
};

struct SupervisorConfig {
    // How long the LLM may stay Unhealthy before pipeline_state flips to
    // failed. Counts from the Unhealthy transition, not from the first
    // failed probe.
    uint32_t pipeline_fail_grace_seconds = 30;
    // Per-probe HTTP timeout. Applied uniformly across services.
    uint32_t probe_timeout_ms = 3000;
};

struct MemorySummaryConfig {
    std::string trigger = "turns";    // turns | tokens | idle | hybrid
    uint32_t turn_threshold = 15;
    uint32_t token_threshold = 4000;
    uint32_t idle_seconds = 120;
    std::string language = "dominant"; // dominant | english | recent
};

struct MemoryFactsConfig {
    std::string policy = "conservative"; // conservative | moderate | aggressive | manual_only
    double confidence_threshold = 0.7;
};

struct MemoryConfig {
    std::string db_path = "${XDG_DATA_HOME:-~/.local/share}/acva/acva.db";
    uint32_t recent_turns_n = 10;
    uint32_t write_queue_capacity = 256;
    MemorySummaryConfig summary;
    MemoryFactsConfig facts;
};

struct SentenceSplitterConfig {
    uint32_t max_sentence_chars = 600; // forced flush threshold
    bool detect_code_fences = true;
};

struct DialogueConfig {
    uint32_t recent_turns_n = 10;
    uint32_t max_assistant_sentences = 6;
    uint32_t max_assistant_tokens = 400;
    std::string fallback_language = "en";
    // Map of lang code → system-prompt text. English is required.
    std::map<std::string, std::string> system_prompts;
    SentenceSplitterConfig sentence_splitter;
};

struct Config {
    LoggingConfig logging;
    ControlConfig control;
    PipelineConfig pipeline;
    LlmConfig llm;
    SttConfig stt;
    TtsConfig tts;
    SupervisorConfig supervisor;
    MemoryConfig memory;
    DialogueConfig dialogue;
};

struct LoadError {
    std::string message;
};

// LoadResult is std::variant<Config, LoadError>. Use std::holds_alternative or
// std::get_if to discriminate. C++20 doesn't have std::expected; this is a
// stable substitute.
using LoadResult = std::variant<Config, LoadError>;

[[nodiscard]] LoadResult load_from_file(const std::filesystem::path& path);
[[nodiscard]] LoadResult load_from_string(std::string_view yaml);

[[nodiscard]] std::optional<LoadError> validate(const Config& cfg);

[[nodiscard]] LogSink parse_log_sink(std::string_view s);

} // namespace acva::config
