#include "playback/queue.hpp"

#include <utility>

namespace acva::playback {

PlaybackQueue::PlaybackQueue(std::size_t max_chunks) : cap_(max_chunks) {}

bool PlaybackQueue::enqueue(AudioChunk chunk) {
    std::lock_guard lk(mu_);
    if (q_.size() >= cap_) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    q_.push_back(std::move(chunk));
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::optional<AudioChunk> PlaybackQueue::dequeue_active(dialogue::TurnId active_turn) {
    std::lock_guard lk(mu_);
    // Skip stale chunks at the head. A turn-id mismatch always means
    // a barge-in or speculation-cancel happened upstream; the audio
    // callback should never play that audio.
    while (!q_.empty() && q_.front().turn != active_turn) {
        q_.pop_front();
        drops_.fetch_add(1, std::memory_order_relaxed);
    }
    if (q_.empty()) return std::nullopt;
    auto chunk = std::move(q_.front());
    q_.pop_front();
    dequeued_.fetch_add(1, std::memory_order_relaxed);
    return chunk;
}

std::size_t PlaybackQueue::invalidate_before(dialogue::TurnId next_turn) {
    std::lock_guard lk(mu_);
    if (q_.empty()) return 0;

    // Walk the deque and rebuild only the survivors. Linear in queue
    // depth which is bounded by `cap_` — fine inside the lock.
    std::deque<AudioChunk> kept;
    std::size_t dropped = 0;
    for (auto& c : q_) {
        if (c.turn < next_turn) {
            ++dropped;
        } else {
            kept.push_back(std::move(c));
        }
    }
    q_ = std::move(kept);
    drops_.fetch_add(dropped, std::memory_order_relaxed);
    return dropped;
}

std::size_t PlaybackQueue::clear() {
    std::lock_guard lk(mu_);
    const auto n = q_.size();
    q_.clear();
    drops_.fetch_add(n, std::memory_order_relaxed);
    return n;
}

std::size_t PlaybackQueue::size() const {
    std::lock_guard lk(mu_);
    return q_.size();
}

std::size_t PlaybackQueue::pending_samples_for(dialogue::TurnId turn) const {
    std::lock_guard lk(mu_);
    std::size_t total = 0;
    for (const auto& c : q_) {
        if (c.turn == turn) total += c.samples.size();
    }
    return total;
}

} // namespace acva::playback
