#include "supervisor/supervisor.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace acva::supervisor {

std::string_view to_string(PipelineState s) noexcept {
    switch (s) {
        case PipelineState::Ok:                            return "ok";
        case PipelineState::Failed:                        return "failed";
        case PipelineState::NoConfiguredCriticalServices:  return "no_configured_critical_services";
    }
    return "unknown";
}

Supervisor::Supervisor(const config::SupervisorConfig& cfg,
                        event::EventBus& bus,
                        ProbeFn probe)
    : cfg_(cfg), bus_(bus), probe_(std::move(probe)) {
    // Subscribe immediately — events that arrive before start() flow
    // through the bus's per-subscription queue and are dispatched once
    // the worker is up. Wakes the evaluator loop on every transition.
    event::SubscribeOptions opts;
    opts.name = "supervisor.health";
    opts.queue_capacity = 64;
    opts.policy = event::OverflowPolicy::DropOldest;
    health_sub_ = bus_.subscribe<event::HealthChanged>(
        std::move(opts),
        [this](const event::HealthChanged&) {
            evaluator_wake_.store(true, std::memory_order_release);
            evaluator_cv_.notify_all();
        });
}

Supervisor::~Supervisor() {
    stop();
}

void Supervisor::register_service(ServiceConfig svc) {
    std::lock_guard lk(mu_);
    // Replace existing entry with the same name (mostly for tests).
    auto it = std::find_if(monitors_.begin(), monitors_.end(),
                            [&](const auto& m){ return m->name() == svc.name; });
    auto monitor = std::make_unique<ServiceMonitor>(std::move(svc), probe_, bus_);
    if (it == monitors_.end()) {
        monitors_.push_back(std::move(monitor));
    } else {
        // We hold mu_; the old monitor's stop() acquires its own internal
        // mutex, so there's no lock-order issue here.
        (*it)->stop();
        *it = std::move(monitor);
    }
}

void Supervisor::start() {
    if (running_.exchange(true)) return;

    {
        std::lock_guard lk(mu_);
        for (auto& m : monitors_) m->start();
    }
    evaluator_ = std::thread([this]{ evaluator_loop(); });
    evaluate_pipeline();
}

void Supervisor::stop() {
    if (!running_.exchange(false)) return;

    evaluator_wake_.store(true, std::memory_order_release);
    evaluator_cv_.notify_all();
    if (evaluator_.joinable()) evaluator_.join();

    {
        std::lock_guard lk(mu_);
        for (auto& m : monitors_) m->stop();
    }
    if (health_sub_) {
        health_sub_->stop();
        health_sub_.reset();
    }
}

SupervisorSnapshot Supervisor::snapshot() const {
    SupervisorSnapshot out;
    out.pipeline_state = pipeline_state_.load(std::memory_order_acquire);
    std::lock_guard lk(mu_);
    out.services.reserve(monitors_.size());
    for (const auto& m : monitors_) {
        out.services.push_back(m->snapshot());
    }
    return out;
}

PipelineState Supervisor::wait_for_pipeline_state(
        PipelineState s, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (pipeline_state_.load(std::memory_order_acquire) != s) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pipeline_state_.load(std::memory_order_acquire);
}

void Supervisor::evaluator_loop() {
    using namespace std::chrono_literals;
    std::mutex local_mu;
    std::unique_lock lk(local_mu);
    while (running_.load(std::memory_order_acquire)) {
        // Wake on HealthChanged or every 1s — whichever comes first.
        // The 1s tick is what eventually trips the grace window for a
        // backend that stays Unhealthy longer than
        // pipeline_fail_grace_seconds without further state changes.
        evaluator_cv_.wait_for(lk, 1s, [this]{
            return evaluator_wake_.load(std::memory_order_acquire)
                || !running_.load(std::memory_order_acquire);
        });
        evaluator_wake_.store(false, std::memory_order_release);
        if (!running_.load(std::memory_order_acquire)) break;
        evaluate_pipeline();
    }
}

void Supervisor::evaluate_pipeline() {
    bool any_critical = false;
    bool any_failed   = false;

    const auto now = std::chrono::steady_clock::now();
    const auto grace = std::chrono::seconds(cfg_.pipeline_fail_grace_seconds);

    std::lock_guard lk(mu_);
    for (const auto& m : monitors_) {
        auto snap = m->snapshot();
        if (!snap.fail_pipeline_if_down) continue;
        // Services with no health_url stay in NotConfigured forever and
        // never gate the pipeline.
        if (snap.state == ServiceState::NotConfigured) continue;
        any_critical = true;

        const bool unhealthy = snap.state == ServiceState::Unhealthy;

        // Track the first time we saw this service Unhealthy (or clear
        // the marker on recovery).
        auto it = std::find_if(unhealthy_since_.begin(), unhealthy_since_.end(),
                                [&](const auto& u){ return u.name == snap.name; });
        if (unhealthy) {
            if (it == unhealthy_since_.end()) {
                unhealthy_since_.push_back({snap.name, now});
                it = unhealthy_since_.end() - 1;
            }
            if (now - it->since >= grace) {
                any_failed = true;
            }
        } else if (it != unhealthy_since_.end()) {
            unhealthy_since_.erase(it);
        }
    }

    PipelineState next;
    if (!any_critical) {
        next = PipelineState::NoConfiguredCriticalServices;
    } else {
        next = any_failed ? PipelineState::Failed : PipelineState::Ok;
    }

    const auto prev = pipeline_state_.exchange(next, std::memory_order_acq_rel);
    if (prev != next) {
        log::event("supervisor", "pipeline_state", event::kNoTurn, {
            {"from", std::string{to_string(prev)}},
            {"to",   std::string{to_string(next)}},
        });
        if (next == PipelineState::Failed) {
            bus_.publish(event::ErrorEvent{
                .component = "supervisor",
                .message   = "pipeline failed: critical service unhealthy past grace",
            });
        }
    }
}

} // namespace acva::supervisor
