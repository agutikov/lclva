#pragma once

#include "event/bus.hpp"
#include "event/event.hpp"

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace acva::metrics {

// Wraps prometheus::Registry and the metric families we use across the
// orchestrator. Constructed once at startup and shared (by reference) by all
// components that emit metrics.
//
// Family names follow the project convention `voice_*` from §12 of the
// design. Labels are chosen for low cardinality (e.g., `event` is one of a
// fixed set of event names).
class Registry {
public:
    Registry();

    // The underlying prometheus registry, exposed for HTTP serialization
    // and ad-hoc registration of additional Collectables.
    [[nodiscard]] std::shared_ptr<prometheus::Registry> registry() const noexcept {
        return registry_;
    }

    // Increment the published-events counter for `event_name`.
    void on_event_published(const char* event_name);

    // Mark a turn outcome. `outcome` is one of: completed, interrupted, discarded.
    void on_turn_outcome(const char* outcome);

    // Drops on a queue (event bus subscription queue, etc.).
    void on_queue_drop(const std::string& queue_name);

    // Service restart count (called by supervisor in M2).
    void on_service_restart(const std::string& service_name);

    // Update an FSM state gauge (1 for current state, 0 elsewhere). Called
    // by the FSM observer.
    void set_fsm_state(const char* state_name);

    // M2 — supervisor health metrics. ----------------------------------

    // Pre-create a service's health-state gauge so /metrics shows all
    // four label values from t=0 onwards. Called once per registered
    // service at startup. Without this, prometheus would only show a
    // service after its state transitions.
    void register_service_for_health(const std::string& service);

    // Set the health gauge for `service` to `state`. Mirrors set_fsm_state.
    void set_health_state(const std::string& service, const char* state);

    // Increment the keep-alive ping counter (fired vs skipped).
    void on_keep_alive(bool fired);

    // Set the pipeline-state gauge (one of: ok, failed,
    // no_configured_critical_services).
    void set_pipeline_state(const char* state);

    // Subscribe metrics-collection handlers to the bus. Call after
    // construction. Returns subscriptions which the caller must keep alive.
    [[nodiscard]] std::vector<event::SubscriptionHandle> subscribe(event::EventBus& bus);

private:
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Family<prometheus::Counter>*   events_published_;
    prometheus::Family<prometheus::Counter>*   turns_total_;
    prometheus::Family<prometheus::Counter>*   queue_drops_;
    prometheus::Family<prometheus::Counter>*   service_restarts_;
    prometheus::Family<prometheus::Counter>*   keep_alive_total_;
    prometheus::Family<prometheus::Gauge>*     fsm_state_;
    prometheus::Family<prometheus::Gauge>*     health_state_;
    prometheus::Family<prometheus::Gauge>*     pipeline_state_;
    prometheus::Family<prometheus::Histogram>* llm_first_token_ms_;
    prometheus::Family<prometheus::Histogram>* llm_tokens_per_sec_;
    prometheus::Histogram* llm_first_token_ms_metric_ = nullptr;
    prometheus::Histogram* llm_tokens_per_sec_metric_ = nullptr;

    // Services registered via register_service_for_health(). Used so
    // set_health_state can clear all stale labels when a service flips.
    std::mutex health_mu_;
    std::vector<std::string> health_services_;

    // Per-turn timing state captured between LlmStarted / LlmToken /
    // LlmFinished. Single mutex covers both maps; events are infrequent
    // relative to other contention.
    struct TurnTimer {
        std::chrono::steady_clock::time_point started_at{};
        bool first_token_seen = false;
    };
    std::mutex timers_mu_;
    std::unordered_map<event::TurnId, TurnTimer> timers_;
};

} // namespace acva::metrics
