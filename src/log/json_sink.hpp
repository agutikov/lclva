#pragma once

#include <spdlog/sinks/base_sink.h>

#include <cstdio>
#include <mutex>
#include <string_view>

namespace acva::log {

// JSON-line sink for spdlog. One JSON object per call, separated by '\n'.
//
// Schema:
//   {"ts": ISO8601 with milliseconds + tz,
//    "level": "trace|debug|info|warn|error|critical",
//    "component": <inferred from "[component] ..." prefix in the log message>,
//    "msg":  <remainder>                              [for plain info/warn/...],
//    "event": <name>, "turn_id": <id>, "k": "v", ...  [for log::event()]}
//
// The sink runs the structured path when it sees the marker
// `\x01EVT\x01<inner-fields>` after the component prefix; otherwise it
// emits the remainder as the "msg" string. The marker is produced only
// by log::event() and never appears in user code.
//
// Thread-safe: base_sink<std::mutex> serializes calls.
class JsonSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit JsonSink(std::FILE* out = stderr) : out_(out) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_()                                          override;

private:
    std::FILE* out_;
};

// Internal: marker for structured-event payloads. Public only so the
// `event()` helper in log.hpp can compose with it; not for user code.
inline constexpr std::string_view kEventMarker = "\x01EVT\x01";

} // namespace acva::log
