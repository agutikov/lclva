#include "log/json_sink.hpp"

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <spdlog/common.h>

#include <chrono>
#include <ctime>
#include <string>
#include <string_view>

namespace lclva::log {

namespace {

// Append a JSON-quoted, properly-escaped string to `out`.
void escape_string(std::string& out, std::string_view s) {
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

std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto ms   = duration_cast<milliseconds>(tp - secs).count();
    const auto t    = system_clock::to_time_t(secs);
    std::tm tm_local{};
    localtime_r(&t, &tm_local);
    return fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}{:+03d}{:02d}",
        tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
        tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
        static_cast<int>(ms),
        static_cast<int>(tm_local.tm_gmtoff / 3600),
        static_cast<int>(std::abs(tm_local.tm_gmtoff % 3600) / 60));
}

std::string_view level_name(spdlog::level::level_enum lvl) noexcept {
    switch (lvl) {
        case spdlog::level::trace:    return "trace";
        case spdlog::level::debug:    return "debug";
        case spdlog::level::info:     return "info";
        case spdlog::level::warn:     return "warn";
        case spdlog::level::err:      return "error";
        case spdlog::level::critical: return "critical";
        default:                       return "off";
    }
}

// Try to split "[component] rest" out of payload.
// Returns true and fills out the slices on success; false leaves them empty.
bool split_component(std::string_view payload,
                     std::string_view& component,
                     std::string_view& rest) noexcept {
    if (payload.empty() || payload.front() != '[') return false;
    auto close = payload.find("] ");
    if (close == std::string_view::npos) return false;
    component = payload.substr(1, close - 1);
    rest = payload.substr(close + 2);
    return true;
}

} // namespace

void JsonSink::sink_it_(const spdlog::details::log_msg& msg) {
    std::string out;
    out.reserve(256);

    out.append("{\"ts\":\"");
    out.append(format_timestamp(msg.time));
    out.append("\",\"level\":\"");
    out.append(level_name(msg.level));
    out.append("\"");

    std::string_view payload(msg.payload.data(), msg.payload.size());

    std::string_view component;
    std::string_view rest;
    if (split_component(payload, component, rest)) {
        out.append(",\"component\":");
        escape_string(out, component);

        if (rest.size() >= kEventMarker.size()
            && rest.compare(0, kEventMarker.size(), kEventMarker) == 0) {
            // Structured event: rest is "<marker><inner-fields-json-frag>"
            // where inner-fields are pre-formatted as ","-separated JSON
            // key:value pairs (no surrounding braces). Splice in directly.
            auto inner = rest.substr(kEventMarker.size());
            if (!inner.empty()) {
                out.push_back(',');
                out.append(inner);
            }
        } else {
            out.append(",\"msg\":");
            escape_string(out, rest);
        }
    } else {
        out.append(",\"msg\":");
        escape_string(out, payload);
    }

    out.append("}\n");

    std::fwrite(out.data(), 1, out.size(), out_);
}

void JsonSink::flush_() {
    std::fflush(out_);
}

} // namespace lclva::log
