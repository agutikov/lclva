#include "log/log.hpp"

#include "log/json_sink.hpp"

#include <fmt/format.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace acva::log {

namespace {

constexpr const char* kLoggerName = "acva";

// Pattern used by the JSON sink: emit only the formatted user payload.
// All structural framing (ts, level, component) is added by the sink
// itself reading log_msg, so the spdlog formatter must not duplicate it.
constexpr const char* kJsonPattern = "%v";

spdlog::level::level_enum parse_level(std::string_view level) {
    if (level == "trace")    return spdlog::level::trace;
    if (level == "debug")    return spdlog::level::debug;
    if (level == "info")     return spdlog::level::info;
    if (level == "warn")     return spdlog::level::warn;
    if (level == "error")    return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    if (level == "off")      return spdlog::level::off;
    return spdlog::level::info;
}

std::shared_ptr<spdlog::logger> g_logger; // NOLINT(*global*)

} // namespace

void init(const config::LoggingConfig& cfg) {
    spdlog::sink_ptr sink;

    const auto sink_kind = config::parse_log_sink(cfg.sink);
    switch (sink_kind) {
        case config::LogSink::file: {
            if (!cfg.file_path) {
                throw std::runtime_error("log: file_path required for sink=file");
            }
            std::FILE* fp = std::fopen(cfg.file_path->c_str(), "ae");
            if (!fp) {
                throw std::runtime_error(
                    "log: failed to open " + *cfg.file_path);
            }
            sink = std::make_shared<JsonSink>(fp);
            break;
        }
        case config::LogSink::journal:
        case config::LogSink::stderr_:
        default:
            // journald: under systemd, stderr is captured into the
            // journal as-is. JSON-per-line is exactly what `journalctl
            // -o json` and downstream collectors expect.
            sink = std::make_shared<JsonSink>(stderr);
            break;
    }

    sink->set_pattern(kJsonPattern);

    auto new_logger = std::make_shared<spdlog::logger>(kLoggerName, sink);
    new_logger->set_level(parse_level(cfg.level));
    new_logger->flush_on(spdlog::level::warn);

    // Replace global. spdlog::set_default_logger keeps a copy.
    spdlog::set_default_logger(new_logger);
    g_logger = std::move(new_logger);
}

namespace {

// JSON-escape a string into `out`. Mirrors the helper inside json_sink.cpp;
// duplicated here so log.cpp doesn't need to expose it through json_sink.hpp.
void escape_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out.append(fmt::format("\\u{:04x}", static_cast<unsigned char>(c)));
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

} // namespace

void event(std::string_view component,
           std::string_view event_name,
           acva::event::TurnId turn,
           std::initializer_list<std::pair<std::string_view, std::string>> kv) {
    // The sink expects the payload to start with "[<component>] ", so we
    // build the message exactly that way and follow it with the marker.
    std::string payload;
    payload.reserve(64 + kv.size() * 32);
    payload.push_back('[');
    payload.append(component);
    payload.append("] ");
    payload.append(kEventMarker);

    payload.append("\"event\":");
    escape_json_string(payload, event_name);

    if (turn != acva::event::kNoTurn) {
        payload.append(",\"turn_id\":");
        payload.append(std::to_string(turn));
    }
    for (const auto& [k, v] : kv) {
        payload.push_back(',');
        escape_json_string(payload, k);
        payload.push_back(':');
        escape_json_string(payload, v);
    }

    SPDLOG_LOGGER_INFO(logger(), "{}", payload);
}

std::shared_ptr<spdlog::logger> logger() {
    if (!g_logger) {
        // Defensive: someone logged before init(). Hand back a stderr fallback
        // so we don't crash; init() will replace it.
        auto fallback = spdlog::default_logger();
        if (!fallback) {
            fallback = spdlog::stderr_color_mt("acva-fallback");
            fallback->set_pattern(kJsonPattern);
        }
        return fallback;
    }
    return g_logger;
}

} // namespace acva::log
