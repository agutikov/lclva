#pragma once

#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "metrics/registry.hpp"

#include <memory>

namespace lclva::http {

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
    ControlServer(const config::ControlConfig& cfg,
                  std::shared_ptr<metrics::Registry> registry,
                  const dialogue::Fsm* fsm);
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
    std::unique_ptr<Impl> impl_;
    int bound_port_ = 0;
};

} // namespace lclva::http
