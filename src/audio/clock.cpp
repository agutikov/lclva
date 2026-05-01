#include "audio/clock.hpp"

namespace acva::audio {

namespace {

std::int64_t to_ns(std::chrono::steady_clock::time_point tp) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch()).count();
}

std::chrono::steady_clock::time_point from_ns(std::int64_t ns) noexcept {
    return std::chrono::steady_clock::time_point{
        std::chrono::nanoseconds{ns}};
}

} // namespace

void MonotonicAudioClock::on_capture_frames(std::uint64_t frame_count,
                                             std::uint32_t sample_rate) noexcept {
    if (sample_rate == 0 || frame_count == 0) return;

    if (!anchored_.load(std::memory_order_acquire)) {
        // First tick — anchor at "now" with frame index 0.
        sample_rate_.store(sample_rate, std::memory_order_release);
        anchor_frame_.store(0, std::memory_order_release);
        anchor_steady_ns_.store(
            to_ns(std::chrono::steady_clock::now()), std::memory_order_release);
        total_frames_.store(frame_count, std::memory_order_release);
        anchored_.store(true, std::memory_order_release);
        return;
    }

    // Re-anchor each tick at the buffer-end timestamp. This bounds drift
    // between the audio device's own clock and steady_clock to a single
    // buffer's worth of frames, which is what M6's AEC alignment requires.
    const auto prev_total = total_frames_.fetch_add(
        frame_count, std::memory_order_acq_rel);
    const auto new_total = prev_total + frame_count;
    anchor_frame_.store(new_total, std::memory_order_release);
    anchor_steady_ns_.store(
        to_ns(std::chrono::steady_clock::now()), std::memory_order_release);
}

std::chrono::steady_clock::time_point
MonotonicAudioClock::steady_for(std::uint64_t frame_index) const noexcept {
    if (!anchored_.load(std::memory_order_acquire)) {
        return std::chrono::steady_clock::time_point{};
    }
    const auto sr = sample_rate_.load(std::memory_order_acquire);
    if (sr == 0) {
        return std::chrono::steady_clock::time_point{};
    }
    const auto anchor_frame = anchor_frame_.load(std::memory_order_acquire);
    const auto anchor_ns    = anchor_steady_ns_.load(std::memory_order_acquire);

    const std::int64_t delta_frames =
        static_cast<std::int64_t>(frame_index) -
        static_cast<std::int64_t>(anchor_frame);
    const std::int64_t ns_per_frame =
        static_cast<std::int64_t>(1'000'000'000) / static_cast<std::int64_t>(sr);
    return from_ns(anchor_ns + delta_frames * ns_per_frame);
}

std::uint64_t
MonotonicAudioClock::frames_at(std::chrono::steady_clock::time_point tp) const noexcept {
    if (!anchored_.load(std::memory_order_acquire)) return 0;
    const auto sr = sample_rate_.load(std::memory_order_acquire);
    if (sr == 0) return 0;
    const auto anchor_frame = anchor_frame_.load(std::memory_order_acquire);
    const auto anchor_ns    = anchor_steady_ns_.load(std::memory_order_acquire);

    const std::int64_t delta_ns = to_ns(tp) - anchor_ns;
    const std::int64_t delta_frames =
        delta_ns * static_cast<std::int64_t>(sr) / 1'000'000'000;
    const std::int64_t result =
        static_cast<std::int64_t>(anchor_frame) + delta_frames;
    return result < 0 ? 0 : static_cast<std::uint64_t>(result);
}

} // namespace acva::audio
