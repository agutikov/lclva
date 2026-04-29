#pragma once

#include "config/config.hpp"

#include <spdlog/spdlog.h>

#include <memory>
#include <string_view>

namespace lclva::log {

// Initialize the global logger from config. Must be called once at startup.
// Subsequent calls re-init in place (intended for hot reload of log level).
//
// At M0 this emits a structured plain-text format (ISO ts | level | component
// | message). When the event bus and per-turn trace IDs land in M1/M0-late,
// this switches to a custom JSON sink emitting the schema documented in
// project_design.md §4.14 / §12.
void init(const config::LoggingConfig& cfg);

// Returns the shared application logger. Always non-null after init().
[[nodiscard]] std::shared_ptr<spdlog::logger> logger();

// Convenience helpers. Component is a short tag like "main", "config",
// "supervisor", "dialogue". Message follows.
// SPDLOG_* macros may compile to no-ops at certain build levels, leaving
// parameters technically unused. Mark them [[maybe_unused]] so -Wunused-parameter
// stays clean across all build types.
inline void info([[maybe_unused]] std::string_view component,
                 [[maybe_unused]] std::string_view message) {
    SPDLOG_LOGGER_INFO(logger(), "[{}] {}", component, message);
}

inline void warn([[maybe_unused]] std::string_view component,
                 [[maybe_unused]] std::string_view message) {
    SPDLOG_LOGGER_WARN(logger(), "[{}] {}", component, message);
}

inline void error([[maybe_unused]] std::string_view component,
                  [[maybe_unused]] std::string_view message) {
    SPDLOG_LOGGER_ERROR(logger(), "[{}] {}", component, message);
}

inline void debug([[maybe_unused]] std::string_view component,
                  [[maybe_unused]] std::string_view message) {
    SPDLOG_LOGGER_DEBUG(logger(), "[{}] {}", component, message);
}

} // namespace lclva::log
