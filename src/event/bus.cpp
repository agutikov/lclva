#include "event/bus.hpp"

#include <utility>

namespace lclva::event {

// ===== Subscription =====

Subscription::Subscription(std::string name, std::type_index target,
                           std::function<void(const Event&)> handler,
                           std::size_t queue_capacity, OverflowPolicy policy)
    : name_(std::move(name)),
      target_(target),
      handler_(std::move(handler)),
      queue_(std::make_shared<BoundedQueue<Event>>(queue_capacity, policy)) {
    worker_ = std::thread([this] { run(); });
}

Subscription::~Subscription() {
    stop();
}

void Subscription::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // already stopped
    }
    queue_->close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Subscription::run() {
    while (running_.load(std::memory_order_acquire)) {
        auto evt = queue_->pop();
        if (!evt) {
            // queue closed and drained
            return;
        }
        // Handlers should not throw; if one does, swallow rather than crash
        // the worker. The bus log line is the user-facing trace.
        try {
            handler_(*evt);
        } catch (...) {
            // Subscriber exceptions are isolated from the bus.
        }
    }
}

// ===== EventBus =====

EventBus::~EventBus() {
    shutdown();
}

SubscriptionHandle EventBus::register_sub(SubscribeOptions opts,
                                          std::type_index target,
                                          std::function<void(const Event&)> handler) {
    auto sub = std::make_shared<Subscription>(
        std::move(opts.name), target, std::move(handler),
        opts.queue_capacity, opts.policy);
    {
        std::lock_guard lk(mu_);
        subs_.push_back(sub);
    }
    return sub;
}

void EventBus::publish_event(Event e) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }
    published_.fetch_add(1, std::memory_order_relaxed);

    // Snapshot subscriber list under lock; then push without lock to avoid
    // deadlocks in handlers that touch the bus. Range-init to avoid a
    // gcc-15 false positive on -Werror=null-dereference inside the deeply
    // inlined vector copy-assignment.
    auto snapshot = [this] {
        std::lock_guard lk(mu_);
        return std::vector<SubscriptionHandle>{subs_.begin(), subs_.end()};
    }();

    const std::type_index event_type{typeid(Event)};
    auto active_index = e.index();
    (void)active_index; // reserved for future per-alternative routing optimization

    for (auto& sub : snapshot) {
        if (sub->target() == event_type) {
            // subscribe_all: every event delivered.
            (void)sub->queue().push(e);
            continue;
        }

        // Per-type subscription: dispatch only when the active variant
        // matches the subscriber's target. We compare by visiting and
        // checking the visited type's typeid.
        bool matches = std::visit([&](const auto& v) {
            return std::type_index(typeid(v)) == sub->target();
        }, e);

        if (matches) {
            (void)sub->queue().push(e);
        }
    }
}

void EventBus::shutdown() {
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true)) {
        return;
    }
    std::vector<SubscriptionHandle> snapshot;
    {
        std::lock_guard lk(mu_);
        snapshot.swap(subs_);
    }
    // Closing each subscription drains its queue and joins the worker.
    for (auto& sub : snapshot) {
        sub->stop();
    }
}

std::size_t EventBus::subscriber_count() const {
    std::lock_guard lk(mu_);
    return subs_.size();
}

} // namespace lclva::event
