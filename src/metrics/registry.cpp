#include "metrics/registry.hpp"

#include "dialogue/fsm.hpp" // for State to_string

#include <prometheus/labels.h>

#include <array>
#include <cstring>
#include <utility>

namespace lclva::metrics {

namespace {

// All FSM states we know about. Used to pre-create gauge instances so the
// metric family always has a complete set of labels (avoids the "metric
// disappears when its label hasn't been seen yet" problem).
constexpr std::array kAllStates = {
    "idle", "listening", "user_speaking", "transcribing",
    "thinking", "speaking", "completed", "interrupted",
};

} // namespace

Registry::Registry() : registry_(std::make_shared<prometheus::Registry>()) {
    events_published_ = &prometheus::BuildCounter()
        .Name("voice_events_published_total")
        .Help("Total events published on the control bus, labeled by event type")
        .Register(*registry_);

    turns_total_ = &prometheus::BuildCounter()
        .Name("voice_turns_total")
        .Help("Total turns by terminal outcome")
        .Register(*registry_);

    queue_drops_ = &prometheus::BuildCounter()
        .Name("voice_events_dropped_total")
        .Help("Events dropped due to queue overflow, labeled by queue name")
        .Register(*registry_);

    service_restarts_ = &prometheus::BuildCounter()
        .Name("voice_service_restarts_total")
        .Help("External service restarts, labeled by service name")
        .Register(*registry_);

    fsm_state_ = &prometheus::BuildGauge()
        .Name("voice_fsm_state")
        .Help("Current FSM state — 1 for the active state, 0 for others")
        .Register(*registry_);

    for (auto* s : kAllStates) {
        fsm_state_->Add({{"state", s}}).Set(0.0);
    }
}

void Registry::on_event_published(const char* event_name) {
    events_published_->Add({{"event", event_name}}).Increment();
}

void Registry::on_turn_outcome(const char* outcome) {
    turns_total_->Add({{"outcome", outcome}}).Increment();
}

void Registry::on_queue_drop(const std::string& queue_name) {
    queue_drops_->Add({{"queue", queue_name}}).Increment();
}

void Registry::on_service_restart(const std::string& service_name) {
    service_restarts_->Add({{"service", service_name}}).Increment();
}

void Registry::set_fsm_state(const char* state_name) {
    for (auto* s : kAllStates) {
        const double v = (std::strcmp(s, state_name) == 0) ? 1.0 : 0.0;
        fsm_state_->Add({{"state", s}}).Set(v);
    }
}

std::vector<event::SubscriptionHandle> Registry::subscribe(event::EventBus& bus) {
    std::vector<event::SubscriptionHandle> handles;

    event::SubscribeOptions opts;
    opts.name = "metrics.events";
    opts.queue_capacity = 4096;
    opts.policy = event::OverflowPolicy::DropOldest; // metrics are lossy

    handles.push_back(bus.subscribe_all(opts, [this](const event::Event& e) {
        on_event_published(event::event_name(e));
    }));

    return handles;
}

} // namespace lclva::metrics
