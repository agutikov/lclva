#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace acva::audio {

// Stores a rolling window of the int16 samples the playback engine has
// just emitted to the device, so the M6 AEC wrapper can pull the
// reference signal that corresponds to a given mic capture instant.
//
// Producer: PlaybackEngine::render_into. After it has filled the
// PortAudio output buffer (and immediately before the data physically
// goes to the device), it calls on_emitted() with the chunk it just
// produced and the wall-clock instant the chunk will start playing.
//
// Consumer: the APM wrapper running on the audio-processing thread.
// On each captured 10 ms mic frame it calls aligned(capture_time, N) to
// pull N samples whose first sample was emitted at capture_time. Caller
// supplies a fixed delay (the speaker→air→mic round-trip estimate) to
// shift capture_time backwards before passing it in — see
// plans/milestones/m6_aec.md §4.
//
// The reference samples are stored at native playback rate (48 kHz).
// The wrapper resamples 48 → 16 kHz on the fly in M6 Step 2; we don't
// do it here because the loopback ring is also useful at full rate for
// debug recordings and dump tooling.
//
// Threading: SPSC. on_emitted holds a brief mutex to update the
// (frame_count, emit_time) anchor atomically with the ring write
// position; aligned() acquires the same mutex (microseconds; matches
// the existing PlaybackQueue::dequeue_active contention model). The
// playback callback's worst-case is one uncontended futex syscall per
// chunk — same realtime budget as before.
class LoopbackSink {
public:
    LoopbackSink(std::size_t capacity_samples,
                 std::uint32_t sample_rate_hz) noexcept;

    LoopbackSink(const LoopbackSink&)            = delete;
    LoopbackSink& operator=(const LoopbackSink&) = delete;
    LoopbackSink(LoopbackSink&&)                 = delete;
    LoopbackSink& operator=(LoopbackSink&&)      = delete;

    // Append a chunk to the ring. emit_time is the wall-clock instant
    // at which the FIRST sample of the chunk starts playing.
    void on_emitted(std::span<const std::int16_t> samples,
                    std::chrono::steady_clock::time_point emit_time);

    // Pull `dest.size()` samples whose first sample was emitted at
    // `capture_time`. Zero-fills any portion of the window that is
    // outside the retained ring (either still-future or aged out).
    // Always returns; never blocks beyond the brief anchor mutex.
    void aligned(std::chrono::steady_clock::time_point capture_time,
                 std::span<std::int16_t> dest) const;

    // Convenience overload for callers that want an owned buffer.
    [[nodiscard]] std::vector<std::int16_t>
    aligned(std::chrono::steady_clock::time_point capture_time,
            std::size_t samples_needed) const;

    [[nodiscard]] std::uint32_t sample_rate_hz() const noexcept { return sample_rate_hz_; }
    [[nodiscard]] std::size_t   capacity_samples() const noexcept { return ring_.size(); }

    // Total samples ever written. Used by metrics + tests.
    [[nodiscard]] std::uint64_t total_frames_emitted() const noexcept;

    // True iff at least one chunk has been written.
    [[nodiscard]] bool has_data() const noexcept;

    // Reset the ring (for tests and turn boundaries if we ever decide
    // to wipe stale references). Idempotent.
    void clear() noexcept;

private:
    const std::uint32_t sample_rate_hz_;

    mutable std::mutex          mu_;
    std::vector<std::int16_t>   ring_;
    // Index of the next sample to write modulo ring_.size().
    std::size_t                 write_pos_ = 0;
    // Total samples ever written. Combined with last_emit_time_ this
    // gives a reverse map from wall-clock to frame index.
    std::uint64_t               total_frames_ = 0;
    // Wall-clock instant at which the FIRST sample of the most recent
    // chunk was emitted. Zero until the first on_emitted() call.
    std::chrono::steady_clock::time_point last_emit_time_{};
    // Frame index of the FIRST sample of the most recent chunk; equals
    // (total_frames_ - last_chunk_size_) once last_emit_time_ is set.
    std::uint64_t               last_emit_first_frame_ = 0;
};

} // namespace acva::audio
