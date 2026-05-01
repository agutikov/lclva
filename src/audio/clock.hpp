#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace acva::audio {

// Single monotonic clock anchored at the capture frame counter.
//
// PortAudio's input callback fires with a deterministic number of
// frames per call; we count them as they come in and use that count as
// the project's authoritative time base for audio. Every audio frame —
// captured, resampled, or pushed to the output stream — carries a
// timestamp derived from this clock. The translation between frame
// indices and steady_clock::time_point is anchored on the first call
// to `on_capture_frames` (so steady_for(0) == that wall-clock instant)
// and recalibrated on every subsequent call to absorb buffer-time drift
// — see Step 1 of plans/milestones/m4_audio_vad.md.
//
// AEC reference alignment in M6 needs both streams sharing one
// time base. The capture path drives the clock; the playback path reads
// `frames_at()` to align its own enqueue timestamps to the same axis.
//
// All methods are thread-safe (relaxed atomics; the only writer is the
// capture audio thread, readers are downstream consumers).
class MonotonicAudioClock {
public:
    MonotonicAudioClock() = default;

    // Audio-thread entry point. Called from the capture callback after
    // each input buffer arrives. Increments the frame counter by
    // `frame_count` and updates the wall-clock anchor so steady_for()
    // tracks real time. `sample_rate` may change at most once (when the
    // device opens); subsequent mismatches are ignored.
    void on_capture_frames(std::uint64_t frame_count,
                            std::uint32_t sample_rate) noexcept;

    // Wall-clock instant corresponding to the *start* of frame
    // `frame_index`. Returns time_point{} if the clock has never been
    // ticked.
    [[nodiscard]] std::chrono::steady_clock::time_point
        steady_for(std::uint64_t frame_index) const noexcept;

    // Inverse: frame index closest to the given wall-clock instant.
    [[nodiscard]] std::uint64_t
        frames_at(std::chrono::steady_clock::time_point) const noexcept;

    [[nodiscard]] std::uint32_t sample_rate() const noexcept {
        return sample_rate_.load(std::memory_order_acquire);
    }

    // Total frames seen since construction. Used by capture metrics.
    [[nodiscard]] std::uint64_t total_frames() const noexcept {
        return total_frames_.load(std::memory_order_acquire);
    }

private:
    // Anchor: at wall-clock `anchor_steady_ns_` we had `anchor_frame_`
    // captured frames. steady_for() projects forward at sample_rate_.
    std::atomic<std::uint64_t> total_frames_{0};
    std::atomic<std::uint32_t> sample_rate_{0};
    std::atomic<std::uint64_t> anchor_frame_{0};
    std::atomic<std::int64_t>  anchor_steady_ns_{0};
    std::atomic<bool>          anchored_{false};
};

} // namespace acva::audio
