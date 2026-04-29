#pragma once

#include "event/event.hpp"
#include "event/queue.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace lclva::event {

// One subscription. Owned by the bus; the public Handle is a non-owning
// reference returned to the caller for unsubscribing.
//
// Each Subscription has its own bounded queue and dedicated worker thread.
// The bus dispatcher is the producer; the worker thread drains the queue and
// invokes the handler synchronously.
class Subscription {
public:
    Subscription(std::string name, std::type_index target,
                 std::function<void(const Event&)> handler,
                 std::size_t queue_capacity, OverflowPolicy policy);
    ~Subscription();

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&&) = delete;
    Subscription& operator=(Subscription&&) = delete;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::type_index target() const noexcept { return target_; }
    [[nodiscard]] BoundedQueue<Event>& queue() noexcept { return *queue_; }

    // Stop the worker. Drains the queue first, then joins. Idempotent.
    void stop();

private:
    void run();

    std::string name_;
    std::type_index target_; // typeid(Event) means "all events"
    std::function<void(const Event&)> handler_;
    std::shared_ptr<BoundedQueue<Event>> queue_;
    std::thread worker_;
    std::atomic<bool> running_{true};
};

using SubscriptionHandle = std::shared_ptr<Subscription>;

// Configuration for a subscription.
struct SubscribeOptions {
    std::string name = "anonymous";
    std::size_t queue_capacity = 256;
    OverflowPolicy policy = OverflowPolicy::DropOldest;
};

// Pub/sub bus for typed events. Thread-safe.
//
// Subscribers register either for a specific event type (subscribe<E>(...))
// or for the entire Event variant (subscribe_all(...)). The bus dispatches
// each published event to every interested subscriber's queue. Each
// subscription drains its queue on its own worker thread.
class EventBus {
public:
    EventBus() = default;
    ~EventBus();

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    template <class E>
    SubscriptionHandle subscribe(SubscribeOptions opts,
                                 std::function<void(const E&)> handler) {
        static_assert(!std::is_same_v<E, Event>,
                      "use subscribe_all() to subscribe to the full Event variant");
        auto wrapped = [h = std::move(handler)](const Event& evt) {
            if (auto* typed = std::get_if<E>(&evt)) {
                h(*typed);
            }
        };
        return register_sub(std::move(opts), typeid(E), std::move(wrapped));
    }

    SubscriptionHandle subscribe_all(SubscribeOptions opts,
                                     std::function<void(const Event&)> handler) {
        return register_sub(std::move(opts), typeid(Event), std::move(handler));
    }

    // Publish. The event is forwarded to every matching subscription queue.
    template <class E>
    void publish(E e) {
        publish_event(Event{std::move(e)});
    }

    // Publish a pre-constructed Event variant.
    void publish_event(Event e);

    // Stop all subscriptions and join workers. Subsequent publishes are
    // accepted but not delivered (queues are closed). Idempotent.
    void shutdown();

    [[nodiscard]] std::size_t subscriber_count() const;

    // Total events published; useful for tests.
    [[nodiscard]] std::uint64_t total_published() const noexcept {
        return published_.load(std::memory_order_relaxed);
    }

private:
    SubscriptionHandle register_sub(SubscribeOptions opts,
                                    std::type_index target,
                                    std::function<void(const Event&)> handler);

    mutable std::mutex mu_;
    std::vector<SubscriptionHandle> subs_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<std::uint64_t> published_{0};
};

} // namespace lclva::event
