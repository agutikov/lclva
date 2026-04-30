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

    llm_first_token_ms_ = &prometheus::BuildHistogram()
        .Name("voice_llm_first_token_ms")
        .Help("Time from LlmStarted to first LlmToken, milliseconds")
        .Register(*registry_);
    // Buckets: cover sub-100ms (cache-warm) up to multi-second (cold/long).
    llm_first_token_ms_metric_ = &llm_first_token_ms_->Add({},
        prometheus::Histogram::BucketBoundaries{
            50, 100, 200, 350, 500, 750, 1000, 1500, 2000, 3000, 5000, 10000});

    llm_tokens_per_sec_ = &prometheus::BuildHistogram()
        .Name("voice_llm_tokens_per_sec")
        .Help("LLM token generation rate per turn (tokens / stream duration)")
        .Register(*registry_);
    llm_tokens_per_sec_metric_ = &llm_tokens_per_sec_->Add({},
        prometheus::Histogram::BucketBoundaries{
            5, 10, 20, 40, 80, 120, 200, 400});
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

    {
        event::SubscribeOptions opts;
        opts.name = "metrics.events";
        opts.queue_capacity = 4096;
        opts.policy = event::OverflowPolicy::DropOldest;
        handles.push_back(bus.subscribe_all(opts, [this](const event::Event& e) {
            on_event_published(event::event_name(e));
        }));
    }

    // LLM-timing fan-out. Each turn's start time is recorded on
    // LlmStarted; first LlmToken records first-token latency; LlmFinished
    // records tokens-per-second over the whole stream and clears the
    // entry. A single subscription serializes all three so the
    // per-turn map needs only the trailing erase under the mutex.
    {
        event::SubscribeOptions opts;
        opts.name = "metrics.llm";
        opts.queue_capacity = 1024;
        opts.policy = event::OverflowPolicy::DropOldest;
        handles.push_back(bus.subscribe_all(opts, [this](const event::Event& e) {
            std::visit([this]<class T>(const T& evt) {
                using namespace std::chrono;
                if constexpr (std::is_same_v<T, event::LlmStarted>) {
                    std::lock_guard lk(timers_mu_);
                    timers_[evt.turn] = TurnTimer{
                        .started_at = steady_clock::now(),
                        .first_token_seen = false,
                    };
                } else if constexpr (std::is_same_v<T, event::LlmToken>) {
                    auto now = steady_clock::now();
                    double ms = -1.0;
                    {
                        std::lock_guard lk(timers_mu_);
                        auto it = timers_.find(evt.turn);
                        if (it == timers_.end() || it->second.first_token_seen) return;
                        it->second.first_token_seen = true;
                        ms = duration<double, std::milli>(
                                 now - it->second.started_at).count();
                    }
                    if (ms >= 0.0 && llm_first_token_ms_metric_) {
                        llm_first_token_ms_metric_->Observe(ms);
                    }
                } else if constexpr (std::is_same_v<T, event::LlmFinished>) {
                    auto now = steady_clock::now();
                    double tps = 0.0;
                    {
                        std::lock_guard lk(timers_mu_);
                        auto it = timers_.find(evt.turn);
                        if (it == timers_.end()) return;
                        const auto secs = duration<double>(
                                              now - it->second.started_at).count();
                        if (secs > 0.0 && evt.tokens_generated > 0) {
                            tps = static_cast<double>(evt.tokens_generated) / secs;
                        }
                        timers_.erase(it);
                    }
                    if (tps > 0.0 && llm_tokens_per_sec_metric_) {
                        llm_tokens_per_sec_metric_->Observe(tps);
                    }
                }
            }, e);
        }));
    }

    return handles;
}

} // namespace lclva::metrics
