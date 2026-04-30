#pragma once

#include "config/config.hpp"
#include "event/event.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lclva::log {

// Initialize the global logger from config. Must be called once at startup.
// Subsequent calls re-init in place (intended for hot reload of log level).
//
// As of M1 slice 3 the active sink emits one JSON object per line,
// matching the schema documented in project_design.md §4.14 / §12.
// Structured events are produced via log::event(); plain log::info/warn/...
// calls emit a {"msg": ...} field.
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

// Structured-event API. Emits a JSON line of the form
//   {"ts":..., "level":"info", "component":"<component>",
//    "event":"<event_name>", "turn_id":<turn>, "<k>":"<v>", ...}
//
// Pass kNoTurn as `turn` to omit the turn_id field.
//
// Values are JSON-escaped; keys are emitted as-is (use simple identifiers).
// All values are stringified — wrap numeric strings yourself if you want
// downstream parsers to see them as numbers (the JSON sink keeps numeric
// fields like turn_id as integers automatically).
void event(std::string_view component,
           std::string_view event_name,
           event::TurnId turn,
           std::initializer_list<std::pair<std::string_view, std::string>> kv);

} // namespace lclva::log
