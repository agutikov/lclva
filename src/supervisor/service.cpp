#include "supervisor/service.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <utility>

namespace acva::supervisor {

std::string_view to_string(ServiceState s) noexcept {
    switch (s) {
        case ServiceState::NotConfigured: return "not_configured";
        case ServiceState::Probing:       return "probing";
        case ServiceState::Healthy:       return "healthy";
        case ServiceState::Degraded:      return "degraded";
        case ServiceState::Unhealthy:     return "unhealthy";
    }
    return "unknown";
}

event::HealthState to_health_state(ServiceState s) noexcept {
    switch (s) {
        case ServiceState::NotConfigured:
        case ServiceState::Probing:    return event::HealthState::Unknown;
        case ServiceState::Healthy:    return event::HealthState::Healthy;
        case ServiceState::Degraded:   return event::HealthState::Degraded;
        case ServiceState::Unhealthy:  return event::HealthState::Unhealthy;
    }
    return event::HealthState::Unknown;
}

ServiceConfig ServiceConfig::from_health(std::string name,
                                          const config::ServiceHealthConfig& h) {
    return ServiceConfig{
        .name                    = std::move(name),
        .health_url              = h.health_url,
        .fail_pipeline_if_down   = h.fail_pipeline_if_down,
        .probe_interval_healthy  = std::chrono::milliseconds(h.probe_interval_healthy_ms),
        .probe_interval_degraded = std::chrono::milliseconds(h.probe_interval_degraded_ms),
        .degraded_max_failures   = h.degraded_max_failures,
    };
}

ServiceMonitor::ServiceMonitor(ServiceConfig cfg, ProbeFn probe, event::EventBus& bus)
    : cfg_(std::move(cfg)), probe_(std::move(probe)), bus_(bus) {
    if (cfg_.health_url.empty()) {
        state_.store(ServiceState::NotConfigured, std::memory_order_release);
    } else {
        state_.store(ServiceState::Probing, std::memory_order_release);
    }
}

ServiceMonitor::~ServiceMonitor() {
    stop();
}

void ServiceMonitor::start() {
    if (cfg_.health_url.empty()) return;     // dormant — see ctor
    if (running_.exchange(true)) return;     // already started
    stopping_.store(false, std::memory_order_release);
    worker_ = std::thread([this] { run_loop(); });
}

void ServiceMonitor::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lk(mu_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

ServiceState ServiceMonitor::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

ServiceSnapshot ServiceMonitor::snapshot() const {
    std::lock_guard lk(mu_);
    return ServiceSnapshot{
        .name                    = cfg_.name,
        .state                   = state_.load(std::memory_order_acquire),
        .last_probe_at           = last_probe_at_,
        .last_ok_at              = last_ok_at_,
        .consecutive_failures    = consecutive_failures_,
        .total_probes            = total_probes_,
        .total_failures          = total_failures_,
        .last_http_status        = last_http_status_,
        .last_error              = last_error_,
        .last_latency            = last_latency_,
        .fail_pipeline_if_down   = cfg_.fail_pipeline_if_down,
    };
}

bool ServiceMonitor::wait_for_probe(std::chrono::milliseconds timeout) {
    const auto target = probe_seq_.load(std::memory_order_acquire) + 1;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (probe_seq_.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

std::chrono::milliseconds ServiceMonitor::current_interval() const {
    // Probe more aggressively while the service is anything other than
    // Healthy. Unhealthy uses the same fast interval as Degraded so we
    // notice recovery quickly.
    return state_.load(std::memory_order_acquire) == ServiceState::Healthy
        ? cfg_.probe_interval_healthy
        : cfg_.probe_interval_degraded;
}

void ServiceMonitor::publish_if_changed(ServiceState prev) {
    const auto next = state_.load(std::memory_order_acquire);
    const auto prev_pub = to_health_state(prev);
    const auto next_pub = to_health_state(next);
    if (prev == next) return;

    log::event("supervisor", "health_changed", event::kNoTurn, {
        {"service", cfg_.name},
        {"from",    std::string{to_string(prev)}},
        {"to",      std::string{to_string(next)}},
        {"failures", std::to_string(consecutive_failures_)},
        {"http_status", std::to_string(last_http_status_)},
    });

    // Suppress events on internal-only transitions (Probing → Healthy
    // when the very first probe succeeds is a real change worth
    // publishing; Probing → Unhealthy similarly). The check above
    // catches NotConfigured/Probing collapsing to Unknown.
    if (prev_pub == next_pub) return;
    bus_.publish(event::HealthChanged{
        .service = cfg_.name,
        .state   = next_pub,
        .detail  = last_error_,
    });
}

void ServiceMonitor::apply_result(const ProbeResult& r) {
    ServiceState prev;
    {
        std::lock_guard lk(mu_);
        prev = state_.load(std::memory_order_acquire);
        ++total_probes_;
        last_probe_at_     = std::chrono::steady_clock::now();
        last_http_status_  = r.http_status;
        last_latency_      = r.latency;
        last_error_        = r.body_excerpt;

        if (r.ok) {
            consecutive_failures_ = 0;
            last_ok_at_ = last_probe_at_;
            state_.store(ServiceState::Healthy, std::memory_order_release);
        } else {
            ++consecutive_failures_;
            ++total_failures_;
            const bool over = consecutive_failures_ >= cfg_.degraded_max_failures;
            state_.store(over ? ServiceState::Unhealthy : ServiceState::Degraded,
                         std::memory_order_release);
        }
    }

    publish_if_changed(prev);
    probe_seq_.fetch_add(1, std::memory_order_release);
}

void ServiceMonitor::run_loop() {
    log::info("supervisor", fmt::format("monitor[{}] starting at {}",
        cfg_.name, cfg_.health_url));

    while (running_.load(std::memory_order_acquire)) {
        ProbeResult r;
        try {
            r = probe_(cfg_.health_url);
        } catch (const std::exception& ex) {
            r.ok = false;
            r.body_excerpt = std::string{"probe-fn threw: "} + ex.what();
        } catch (...) {
            r.ok = false;
            r.body_excerpt = "probe-fn threw unknown";
        }

        if (!running_.load(std::memory_order_acquire)) break;
        apply_result(r);

        const auto wait = current_interval();
        std::unique_lock lk(mu_);
        cv_.wait_for(lk, wait, [this]{
            return stopping_.load(std::memory_order_acquire);
        });
        if (stopping_.load(std::memory_order_acquire)) break;
    }

    log::info("supervisor", fmt::format("monitor[{}] stopped", cfg_.name));
}

} // namespace acva::supervisor
