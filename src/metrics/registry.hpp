#pragma once

#include "event/bus.hpp"
#include "event/event.hpp"

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>

#include <memory>
#include <string>

namespace lclva::metrics {

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

    // Subscribe metrics-collection handlers to the bus. Call after
    // construction. Returns subscriptions which the caller must keep alive.
    [[nodiscard]] std::vector<event::SubscriptionHandle> subscribe(event::EventBus& bus);

private:
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Family<prometheus::Counter>* events_published_;
    prometheus::Family<prometheus::Counter>* turns_total_;
    prometheus::Family<prometheus::Counter>* queue_drops_;
    prometheus::Family<prometheus::Counter>* service_restarts_;
    prometheus::Family<prometheus::Gauge>*   fsm_state_;
};

} // namespace lclva::metrics
