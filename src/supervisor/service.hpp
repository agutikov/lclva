#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "supervisor/probe.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace acva::supervisor {

// Internal state of a single service monitor. A superset of
// event::HealthState; NotConfigured/Probing collapse to
// event::HealthState::Unknown when published.
//
//   NotConfigured  →  health_url is empty; the monitor is dormant.
//   Probing        →  before the first probe completes.
//   Healthy        →  last probe succeeded; failure counter is 0.
//   Degraded       →  1..(degraded_max_failures-1) consecutive failures.
//   Unhealthy      →  ≥ degraded_max_failures consecutive failures.
//
// On a successful probe from Unhealthy/Degraded the monitor jumps
// straight back to Healthy (no separate "recovering" state — the
// supervisor's pipeline gate is already debounced by its grace window).
enum class ServiceState : std::uint8_t {
    NotConfigured,
    Probing,
    Healthy,
    Degraded,
    Unhealthy,
};

[[nodiscard]] std::string_view to_string(ServiceState s) noexcept;

// Map internal state to the public event enum used in HealthChanged.
[[nodiscard]] event::HealthState to_health_state(ServiceState s) noexcept;

// Snapshot exposed via Supervisor::snapshot() and the /status endpoint.
struct ServiceSnapshot {
    std::string name;
    ServiceState state = ServiceState::NotConfigured;
    // Wall-clock time of the last probe. steady_clock::time_point::min()
    // when no probe has run.
    std::chrono::steady_clock::time_point last_probe_at{};
    std::chrono::steady_clock::time_point last_ok_at{};
    std::uint32_t consecutive_failures = 0;
    std::uint64_t total_probes = 0;
    std::uint64_t total_failures = 0;
    int last_http_status = 0;
    std::string last_error;
    std::chrono::milliseconds last_latency{0};
    bool fail_pipeline_if_down = false;
};

// Per-service runtime configuration. One ServiceConfig per
// ServiceMonitor — this is just a flat copy of the relevant fields out
// of `config::ServiceHealthConfig` plus a logical name.
struct ServiceConfig {
    std::string name;          // logical: "llm" / "stt" / "tts"
    std::string health_url;    // empty → NotConfigured forever
    bool fail_pipeline_if_down = true;
    std::chrono::milliseconds probe_interval_healthy{5000};
    std::chrono::milliseconds probe_interval_degraded{1000};
    std::uint32_t degraded_max_failures = 3;

    [[nodiscard]] static ServiceConfig from_health(
        std::string name, const config::ServiceHealthConfig& h);
};

// ProbeFn — abstract probe so tests can drive transitions
// deterministically without standing up a server. The default factory
// in supervisor.cpp wraps an HttpProbe. Threadsafe: the monitor calls
// it from its own probe thread, and Supervisor may share a probe
// callback across multiple monitors.
using ProbeFn = std::function<ProbeResult(std::string_view url)>;

// One probe loop per service. Owns its own thread; transitions a state
// machine; publishes event::HealthChanged on every change to the
// public-facing state.
class ServiceMonitor {
public:
    ServiceMonitor(ServiceConfig cfg, ProbeFn probe, event::EventBus& bus);
    ~ServiceMonitor();

    ServiceMonitor(const ServiceMonitor&)            = delete;
    ServiceMonitor& operator=(const ServiceMonitor&) = delete;
    ServiceMonitor(ServiceMonitor&&)                 = delete;
    ServiceMonitor& operator=(ServiceMonitor&&)      = delete;

    // Begin probing. Idempotent. NotConfigured services never start a
    // thread — they remain in NotConfigured until destroyed.
    void start();

    // Stop probing. Drains the probe thread cleanly. Idempotent.
    void stop();

    [[nodiscard]] ServiceSnapshot snapshot() const;
    [[nodiscard]] const std::string& name() const noexcept { return cfg_.name; }
    [[nodiscard]] ServiceState state() const noexcept;

    // Block until the next probe completes (used by tests). Returns
    // true if a probe ran within `timeout`, false on timeout. Safe to
    // call from any thread; never blocks the probe thread itself.
    [[nodiscard]] bool wait_for_probe(std::chrono::milliseconds timeout);

private:
    void run_loop();
    void apply_result(const ProbeResult& r);
    [[nodiscard]] std::chrono::milliseconds current_interval() const;
    void publish_if_changed(ServiceState prev);

    ServiceConfig    cfg_;
    ProbeFn          probe_;
    event::EventBus& bus_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // All written under mu_. Reads use mu_ for snapshot(); state_ is
    // also exposed atomically so callers polling for tests don't need
    // to lock.
    std::atomic<ServiceState> state_{ServiceState::NotConfigured};
    std::chrono::steady_clock::time_point last_probe_at_{};
    std::chrono::steady_clock::time_point last_ok_at_{};
    std::uint32_t consecutive_failures_ = 0;
    std::uint64_t total_probes_ = 0;
    std::uint64_t total_failures_ = 0;
    int last_http_status_ = 0;
    std::string last_error_;
    std::chrono::milliseconds last_latency_{0};

    // Test hook: incremented after every applied probe result.
    std::atomic<std::uint64_t> probe_seq_{0};
};

} // namespace acva::supervisor
