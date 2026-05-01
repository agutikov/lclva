#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace acva::supervisor {

// Result of one probe attempt. `ok` is the only field a caller needs to
// drive a state machine; the rest is diagnostic.
//
//   ok=true   →  HTTP 200 within timeout.
//   ok=false  →  any of: connection refused, DNS failure, timeout,
//                non-2xx response, malformed reply. http_status is 0
//                when no response was received.
struct ProbeResult {
    bool ok = false;
    int http_status = 0;
    std::chrono::milliseconds latency{0};
    // First ~200 chars of the response body on non-2xx, or the
    // libcurl/httplib error string when there was no response. Plain
    // text; never spoken to the user.
    std::string body_excerpt;
};

// Synchronous HTTP GET wrapper.
//
// One HttpProbe instance can be reused across many probe calls — it
// holds no per-call state. cpp-httplib's Client is constructed inside
// get() because it needs the parsed authority each time and the cost
// is negligible relative to the network round-trip.
class HttpProbe {
public:
    explicit HttpProbe(std::chrono::milliseconds timeout);

    // GET `url`. Blocks for up to `timeout()` ms. Threadsafe — multiple
    // ServiceMonitors can hold the same HttpProbe and call get()
    // concurrently from their own probe threads.
    [[nodiscard]] ProbeResult get(std::string_view url) const;

    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return timeout_; }

private:
    std::chrono::milliseconds timeout_;
};

// Split `url` into (scheme://authority, path-with-query). Public so
// tests can verify the parser; the supervisor itself goes through get().
//
// Returns {"", ""} on a URL that is missing the scheme. A URL with no
// path component yields path "/".
struct ParsedUrl {
    std::string authority; // e.g. "http://127.0.0.1:8081"
    std::string path;      // e.g. "/health"
};
[[nodiscard]] ParsedUrl parse_url(std::string_view url);

} // namespace acva::supervisor
