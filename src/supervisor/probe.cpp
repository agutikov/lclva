#include "supervisor/probe.hpp"

#include <httplib.h>

#include <chrono>
#include <string>
#include <string_view>

namespace acva::supervisor {

namespace {

// First N chars of a response body, with binary/control chars replaced
// by '.' so the diagnostic stays log-safe.
std::string excerpt(std::string_view body, std::size_t max_chars = 200) {
    std::string out;
    out.reserve(std::min(body.size(), max_chars));
    for (std::size_t i = 0; i < body.size() && i < max_chars; ++i) {
        const auto c = body[i];
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') {
            out.push_back('.');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

ParsedUrl parse_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return {};
    }
    const auto authority_start = scheme_end + 3;
    const auto path_start = url.find('/', authority_start);
    if (path_start == std::string_view::npos) {
        return ParsedUrl{
            .authority = std::string{url},
            .path      = "/",
        };
    }
    return ParsedUrl{
        .authority = std::string{url.substr(0, path_start)},
        .path      = std::string{url.substr(path_start)},
    };
}

HttpProbe::HttpProbe(std::chrono::milliseconds timeout) : timeout_(timeout) {}

ProbeResult HttpProbe::get(std::string_view url) const {
    ProbeResult r;
    auto parsed = parse_url(url);
    if (parsed.authority.empty()) {
        r.body_excerpt = "probe: invalid url (missing scheme)";
        return r;
    }

    httplib::Client cli(parsed.authority);
    // cpp-httplib's timeouts are split between connect + read. Apply the
    // same value to both so the total wall-clock cap stays close to
    // timeout_ regardless of where the failure mode lives.
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Get(parsed.path);
    const auto t1 = std::chrono::steady_clock::now();
    r.latency = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    if (!res) {
        // Network-level failure (timeout / connection refused / DNS).
        r.body_excerpt = std::string{"probe: "} + httplib::to_string(res.error());
        return r;
    }

    r.http_status = res->status;
    if (res->status >= 200 && res->status < 300) {
        r.ok = true;
        return r;
    }
    r.body_excerpt = excerpt(res->body);
    return r;
}

} // namespace acva::supervisor
