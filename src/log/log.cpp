#include "log/log.hpp"

#include "log/json_sink.hpp"

#include <fmt/format.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

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

// Build a per-run filename like "acva-20260503-150329.log" using the
// process's local-time startup instant. Matches the timezone-aware
// timestamp format that the JSON sink writes inside log records.
std::string per_run_filename(std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "acva-%Y%m%d-%H%M%S.log", &tm);
    return buf;
}

// Open the per-run file under `dir`. Creates `dir` if missing.
// Returns nullptr on failure (caller falls back to stderr) and writes
// a one-line diagnostic to stderr so the user sees why the file sink
// is silent.
std::FILE* open_dir_sink(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr,
            "log: cannot create directory '%s' (%s); falling back to stderr\n",
            dir.c_str(), ec.message().c_str());
        return nullptr;
    }
    const auto name = per_run_filename(std::chrono::system_clock::now());
    const auto path = std::filesystem::path(dir) / name;
    // O_APPEND so concurrent writers from this process (single
    // logger thread, but spdlog may flush from multiple) don't
    // collide; per-run names mean the only contention case is a
    // human re-running acva within the same second, which appends
    // both runs to the same file — acceptable.
    std::FILE* fp = std::fopen(path.c_str(), "ae");
    if (!fp) {
        std::fprintf(stderr,
            "log: cannot open '%s'; falling back to stderr\n",
            path.c_str());
        return nullptr;
    }
    std::fprintf(stderr,
        "log: writing to %s\n", path.c_str());
    return fp;
}

} // namespace

void init(const config::LoggingConfig& cfg) {
    std::vector<spdlog::sink_ptr> sinks;

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
            sinks.push_back(std::make_shared<JsonSink>(fp));
            break;
        }
        case config::LogSink::dir: {
            if (!cfg.dir_path) {
                throw std::runtime_error("log: dir_path required for sink=dir");
            }
            std::FILE* fp = open_dir_sink(*cfg.dir_path);
            sinks.push_back(std::make_shared<JsonSink>(fp ? fp : stderr));
            break;
        }
        case config::LogSink::journal:
        case config::LogSink::stderr_:
        default:
            // journald: under systemd, stderr is captured into the
            // journal as-is. JSON-per-line is exactly what `journalctl
            // -o json` and downstream collectors expect.
            sinks.push_back(std::make_shared<JsonSink>(stderr));
            break;
    }

    // mirror_to_stderr tees every log line to stderr in addition to
    // the primary sink — useful when sink=dir/file and you also want
    // to watch the run live. No-op if the primary sink is already
    // stderr/journal (avoid duplicate lines).
    if (cfg.mirror_to_stderr
        && sink_kind != config::LogSink::stderr_
        && sink_kind != config::LogSink::journal) {
        sinks.push_back(std::make_shared<JsonSink>(stderr));
    }

    for (auto& s : sinks) s->set_pattern(kJsonPattern);

    auto new_logger = std::make_shared<spdlog::logger>(
        kLoggerName, sinks.begin(), sinks.end());
    new_logger->set_level(parse_level(cfg.level));
    new_logger->flush_on(spdlog::level::warn);

    // Replace global. spdlog::set_default_logger keeps a copy.
    spdlog::set_default_logger(new_logger);
    g_logger = std::move(new_logger);

    // Without this, info-level lines sit in the FILE*'s 4 KiB stdio
    // buffer until they overflow or a warn/error line forces a flush
    // (`flush_on` above) — so a tail -f of the per-run log can lag the
    // live process by minutes during quiet stretches. The periodic
    // flusher runs in a single registry-owned thread and calls flush()
    // on every registered logger; safe to call repeatedly (each call
    // replaces the prior interval).
    spdlog::flush_every(std::chrono::seconds(1));
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
