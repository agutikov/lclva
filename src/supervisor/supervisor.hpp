#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acva::supervisor {

// Aggregate pipeline state. The orchestrator uses this to gate the
// dialogue path: when pipeline_state == Failed, the Manager refuses
// new turns (M2.5). When NoConfiguredCriticalServices, no service has
// `fail_pipeline_if_down=true` configured — the gate stays Open
// regardless of whether anything is healthy.
enum class PipelineState : std::uint8_t {
    Ok,        // every fail-pipeline service is Healthy or has not yet
               // been Unhealthy long enough to trip the grace window.
    Failed,    // at least one fail-pipeline service has been Unhealthy
               // for ≥ pipeline_fail_grace_seconds.
    NoConfiguredCriticalServices,
};

[[nodiscard]] std::string_view to_string(PipelineState s) noexcept;

struct SupervisorSnapshot {
    PipelineState pipeline_state = PipelineState::NoConfiguredCriticalServices;
    std::vector<ServiceSnapshot> services;
};

// Supervisor — owns one ServiceMonitor per registered service.
//
// register_service() must be called before start(). After start() the
// supervisor's own evaluator thread re-checks pipeline state on every
// HealthChanged event plus on a 1-second timer (so a backend that
// stays Unhealthy longer than pipeline_fail_grace_seconds eventually
// trips the gate even if no further events arrive).
//
// In dev mode the supervisor never issues restart commands — Compose's
// `restart: unless-stopped` does that. The supervisor is purely an
// observer that publishes HealthChanged on transitions and updates an
// aggregate pipeline_state for the dialogue path to consult.
class Supervisor {
public:
    Supervisor(const config::SupervisorConfig& cfg,
                event::EventBus& bus,
                ProbeFn probe);
    ~Supervisor();

    Supervisor(const Supervisor&)            = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&)                 = delete;
    Supervisor& operator=(Supervisor&&)      = delete;

    // Register a service. Subsequent calls with the same name overwrite
    // the prior monitor (mostly for tests; production registers once).
    void register_service(ServiceConfig svc);

    // Start probing every registered service. Idempotent.
    void start();

    // Stop. Joins all monitor threads. Idempotent.
    void stop();

    [[nodiscard]] SupervisorSnapshot snapshot() const;
    [[nodiscard]] PipelineState pipeline_state() const noexcept {
        return pipeline_state_.load(std::memory_order_acquire);
    }

    // Test hook — block until either the pipeline state matches `s` or
    // `timeout` elapses. Returns the actual state observed.
    [[nodiscard]] PipelineState wait_for_pipeline_state(
        PipelineState s, std::chrono::milliseconds timeout);

private:
    void evaluator_loop();
    void evaluate_pipeline();   // recomputes pipeline_state_; publishes ErrorEvent on flip

    const config::SupervisorConfig& cfg_;
    event::EventBus& bus_;
    ProbeFn probe_;

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<ServiceMonitor>> monitors_;

    // Tracks when each service first transitioned to Unhealthy. Reset
    // when the service leaves Unhealthy. Read by evaluate_pipeline() to
    // measure the grace window.
    struct UnhealthyAt {
        std::string name;
        std::chrono::steady_clock::time_point since;
    };
    std::vector<UnhealthyAt> unhealthy_since_;

    std::atomic<PipelineState> pipeline_state_{PipelineState::NoConfiguredCriticalServices};
    std::atomic<bool> running_{false};

    event::SubscriptionHandle health_sub_;
    std::thread evaluator_;
    std::condition_variable evaluator_cv_;
    std::atomic<bool> evaluator_wake_{false};
};

} // namespace acva::supervisor
