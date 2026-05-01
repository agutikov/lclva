#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace acva::audio {

// Cache-line size — hard-coded to the x86_64 default rather than
// std::hardware_destructive_interference_size because the latter is
// gated under -Werror=interference-size on libstdc++ (its value is
// not ABI-stable across -mtune flags).
inline constexpr std::size_t kCacheLine = 64;

// Single-producer / single-consumer lock-free ring buffer.
//
// Used on the M4 capture path: the PortAudio input callback (producer)
// writes one AudioFrame per push; the audio-processing thread (consumer)
// pops them, runs them through the resampler / VAD / endpointer chain.
//
// Capacity is a compile-time constant so the storage is one contiguous
// array — no allocator hits in the hot path. The storage size MUST be a
// power of two; the masked indices then avoid the `% Capacity`
// instruction.
//
// Memory ordering: the head/tail atomics are the only synchronisation;
// payload writes happen-before the corresponding tail.store(release),
// and payload reads happen-after the corresponding head.load(acquire).
// Producer and consumer pads sit on separate cache lines to avoid
// false-sharing the index pair.
template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 1, "SpscRing capacity must be > 1");
    static_assert((Capacity & (Capacity - 1)) == 0,
                   "SpscRing capacity must be a power of two");
    static_assert(std::is_nothrow_move_constructible_v<T>
                  || std::is_trivially_copyable_v<T>,
                  "SpscRing payload must be nothrow movable / trivially copyable");

public:
    SpscRing() = default;

    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    // Producer side. Returns false if the ring is full (the caller
    // owns the overflow policy — capture drops oldest by re-popping;
    // tests assert false on a full ring).
    bool push(T value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (tail + 1) & kMask;
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        slots_[tail] = std::move(value);
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns nullopt if empty.
    std::optional<T> pop() {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        T out = std::move(slots_[head]);
        head_.store((head + 1) & kMask, std::memory_order_release);
        return out;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // Approximate occupancy. Both indices are loaded independently so the
    // result can be off by one relative to either thread's view; that is
    // intentional — accurate occupancy would require a fence.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (t - h) & kMask;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Producer-only atomic pad.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    // Consumer-only atomic pad.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    // Storage.
    alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

} // namespace acva::audio
