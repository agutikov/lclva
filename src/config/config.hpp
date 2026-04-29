#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace lclva::config {

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

struct Config {
    LoggingConfig logging;
    ControlConfig control;
    PipelineConfig pipeline;
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

} // namespace lclva::config
