#pragma once

#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "metrics/registry.hpp"

#include <functional>
#include <memory>
#include <string>

namespace acva::http {

// Tiny HTTP control plane built on cpp-httplib.
//
// Routes:
//   GET /metrics  — Prometheus exposition format from metrics::Registry
//   GET /status   — JSON snapshot: fsm state, active turn, subscriber count
//   GET /health   — plain-text "ok\n"
//
// More routes (/mute, /unmute, /reload, /new-session, /wipe) land in later
// milestones. The implementation is pimpl'd to keep the cpp-httplib mega-
// header out of every translation unit that includes this.
class ControlServer {
public:
    // Optional JSON-fragment supplier for /status. The fragment is
    // substituted into the top-level JSON object as additional fields,
    // so it must NOT include the surrounding braces — return text like
    //   "\"pipeline_state\":\"ok\",\"services\":[...]"
    // ControlServer prepends a comma when the fragment is non-empty.
    // Decoupled from supervisor.hpp so http/server.hpp doesn't pull
    // supervisor headers into every TU that needs the control plane.
    using StatusExtra = std::function<std::string()>;

    ControlServer(const config::ControlConfig& cfg,
                  std::shared_ptr<metrics::Registry> registry,
                  const dialogue::Fsm* fsm,
                  StatusExtra status_extra = {});
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

    [[nodiscard]] int port() const noexcept { return bound_port_; }

private:
    struct Impl;

    std::shared_ptr<metrics::Registry> registry_;
    const dialogue::Fsm* fsm_; // not owned; nullable
    StatusExtra status_extra_;
    std::unique_ptr<Impl> impl_;
    int bound_port_ = 0;
};

} // namespace acva::http
