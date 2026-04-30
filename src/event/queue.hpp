#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace acva::event {

enum class OverflowPolicy {
    // Realtime-style: full queue drops the oldest item and accepts the new one.
    // Counter increments. Used for audio frames and metrics samples — anything
    // where the freshest data matters more than completeness.
    DropOldest,

    // Segment-style: full queue rejects the new item; older items survive.
    // Used for utterance-segment queues where dropping the freshest sample
    // is preferable to losing a completed utterance.
    DropNewest,

    // Backpressure: full queue blocks the producer until space is free.
    // Used for offline/throughput paths. Don't use on the realtime audio path.
    Block,
};

// Bounded MPMC queue. Multiple producers, multiple consumers (typical use is
// MPSC, but we don't enforce it). Lock-based; the lock-free SPSC ring used
// for audio frames is a different type (lands in M4).
//
// Counters (pushes / pops / drops) are exposed for metrics; reading them is
// best-effort (relaxed atomics) and not synchronized with state changes.
template <typename T>
class BoundedQueue {
public:
    BoundedQueue(std::size_t capacity, OverflowPolicy policy)
        : capacity_(capacity), policy_(policy) {}

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;

    // Push. Returns true if the item was accepted, false if dropped.
    // For OverflowPolicy::Block, blocks until space is free or close() is called.
    [[nodiscard]] bool push(T value) {
        std::unique_lock lk(mu_);
        if (closed_) {
            return false;
        }

        if (q_.size() >= capacity_) {
            switch (policy_) {
                case OverflowPolicy::DropOldest:
                    q_.pop_front();
                    drops_.fetch_add(1, std::memory_order_relaxed);
                    break;
                case OverflowPolicy::DropNewest:
                    drops_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                case OverflowPolicy::Block:
                    not_full_.wait(lk, [this] { return q_.size() < capacity_ || closed_; });
                    if (closed_) {
                        return false;
                    }
                    break;
            }
        }

        q_.push_back(std::move(value));
        pushes_.fetch_add(1, std::memory_order_relaxed);
        not_empty_.notify_one();
        return true;
    }

    // Block until an item is available, the queue is closed, or the deadline
    // has passed. Returns std::nullopt if closed-and-empty or timed out.
    [[nodiscard]] std::optional<T> pop_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lk(mu_);
        if (!not_empty_.wait_until(lk, deadline, [this] { return !q_.empty() || closed_; })) {
            return std::nullopt;
        }
        if (q_.empty()) {
            return std::nullopt; // closed and empty
        }
        T value = std::move(q_.front());
        q_.pop_front();
        pops_.fetch_add(1, std::memory_order_relaxed);
        not_full_.notify_one();
        return value;
    }

    // Block indefinitely; returns nullopt only when closed-and-empty.
    [[nodiscard]] std::optional<T> pop() {
        std::unique_lock lk(mu_);
        not_empty_.wait(lk, [this] { return !q_.empty() || closed_; });
        if (q_.empty()) {
            return std::nullopt;
        }
        T value = std::move(q_.front());
        q_.pop_front();
        pops_.fetch_add(1, std::memory_order_relaxed);
        not_full_.notify_one();
        return value;
    }

    // Try-pop without blocking.
    [[nodiscard]] std::optional<T> try_pop() {
        std::unique_lock lk(mu_);
        if (q_.empty()) {
            return std::nullopt;
        }
        T value = std::move(q_.front());
        q_.pop_front();
        pops_.fetch_add(1, std::memory_order_relaxed);
        not_full_.notify_one();
        return value;
    }

    // Closes the queue. After close(), push() returns false; pop() drains
    // remaining items then returns nullopt; producers blocked on Block-policy
    // push wake up and return false.
    void close() {
        {
            std::lock_guard lk(mu_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lk(mu_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(mu_);
        return q_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] OverflowPolicy policy() const noexcept { return policy_; }

    [[nodiscard]] std::uint64_t pushes() const noexcept { return pushes_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t pops()   const noexcept { return pops_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t drops()  const noexcept { return drops_.load(std::memory_order_relaxed); }

private:
    const std::size_t capacity_;
    const OverflowPolicy policy_;

    mutable std::mutex mu_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> q_;
    bool closed_ = false;

    std::atomic<std::uint64_t> pushes_{0};
    std::atomic<std::uint64_t> pops_{0};
    std::atomic<std::uint64_t> drops_{0};
};

} // namespace acva::event
