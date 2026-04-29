#include "log/log.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <stdexcept>
#include <string>

namespace lclva::log {

namespace {

constexpr const char* kLoggerName = "lclva";
constexpr const char* kPattern = "%Y-%m-%dT%H:%M:%S.%e%z %^%-5l%$ %v";

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
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(*cfg.file_path, /*truncate=*/false);
            break;
        }
        case config::LogSink::journal: {
            // journald sink requires libsystemd; not wired in M0. Until then,
            // fall back to stderr — when the orchestrator runs under systemd
            // its stderr is captured into journald automatically anyway.
            sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            break;
        }
        case config::LogSink::stderr_:
        default:
            sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            break;
    }

    sink->set_pattern(kPattern);

    auto new_logger = std::make_shared<spdlog::logger>(kLoggerName, sink);
    new_logger->set_level(parse_level(cfg.level));
    new_logger->flush_on(spdlog::level::warn);

    // Replace global. spdlog::set_default_logger keeps a copy.
    spdlog::set_default_logger(new_logger);
    g_logger = std::move(new_logger);
}

std::shared_ptr<spdlog::logger> logger() {
    if (!g_logger) {
        // Defensive: someone logged before init(). Hand back a stderr fallback
        // so we don't crash; init() will replace it.
        auto fallback = spdlog::default_logger();
        if (!fallback) {
            fallback = spdlog::stderr_color_mt("lclva-fallback");
            fallback->set_pattern(kPattern);
        }
        return fallback;
    }
    return g_logger;
}

} // namespace lclva::log
