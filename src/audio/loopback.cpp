#include "audio/loopback.hpp"

#include <algorithm>
#include <cstring>

namespace acva::audio {

LoopbackSink::LoopbackSink(std::size_t capacity_samples,
                            std::uint32_t sample_rate_hz) noexcept
    : sample_rate_hz_(sample_rate_hz),
      ring_(capacity_samples > 0 ? capacity_samples : 1, std::int16_t{0}) {}

void LoopbackSink::on_emitted(std::span<const std::int16_t> samples,
                                std::chrono::steady_clock::time_point emit_time) {
    if (samples.empty()) return;

    std::lock_guard lk(mu_);
    const std::size_t cap = ring_.size();
    const std::size_t n = samples.size();

    // If the chunk is larger than the ring, only the tail fits — we
    // store the last `cap` samples and shift the conceptual first-frame
    // index forward accordingly. In practice playback chunk sizes are
    // 10–20 ms (480–960 samples at 48 kHz) and the ring is ~2 s, so
    // this branch is exercised only by adversarial tests.
    const std::size_t to_copy = std::min(n, cap);
    const std::size_t skipped = n - to_copy;

    if (to_copy <= cap - write_pos_) {
        std::memcpy(ring_.data() + write_pos_,
                     samples.data() + skipped,
                     to_copy * sizeof(std::int16_t));
    } else {
        const std::size_t first = cap - write_pos_;
        std::memcpy(ring_.data() + write_pos_,
                     samples.data() + skipped,
                     first * sizeof(std::int16_t));
        std::memcpy(ring_.data(),
                     samples.data() + skipped + first,
                     (to_copy - first) * sizeof(std::int16_t));
    }
    write_pos_ = (write_pos_ + to_copy) % cap;

    last_emit_first_frame_ = total_frames_ + skipped;
    total_frames_ += n;
    last_emit_time_ = emit_time;
}

void LoopbackSink::aligned(std::chrono::steady_clock::time_point capture_time,
                            std::span<std::int16_t> dest) const {
    if (dest.empty()) return;

    std::lock_guard lk(mu_);

    // No data yet — silence.
    if (last_emit_time_ == std::chrono::steady_clock::time_point{}) {
        std::memset(dest.data(), 0, dest.size() * sizeof(std::int16_t));
        return;
    }

    // Map capture_time onto the integer frame axis: frame_index of the
    // sample whose emit-time matches capture_time. Negative deltas
    // (capture_time later than last_emit_time_) map past head.
    const auto delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        capture_time - last_emit_time_).count();
    const std::int64_t sr = sample_rate_hz_;
    // Round-to-nearest, signed.
    const std::int64_t delta_frames =
        (delta_ns >= 0)
            ? (delta_ns * sr + 500'000'000) / 1'000'000'000
            : (delta_ns * sr - 500'000'000) / 1'000'000'000;

    const std::int64_t first_target_frame =
        static_cast<std::int64_t>(last_emit_first_frame_) + delta_frames;

    const std::size_t cap = ring_.size();
    // Ring covers frames [total_frames_ - cap, total_frames_).
    const std::int64_t oldest_frame =
        static_cast<std::int64_t>(total_frames_) -
        static_cast<std::int64_t>(std::min<std::uint64_t>(cap, total_frames_));
    const std::int64_t newest_frame = static_cast<std::int64_t>(total_frames_);

    for (std::size_t i = 0; i < dest.size(); ++i) {
        const std::int64_t f = first_target_frame + static_cast<std::int64_t>(i);
        if (f < oldest_frame || f >= newest_frame) {
            dest[i] = 0;
            continue;
        }
        // Map global frame index → ring slot. write_pos_ is the slot
        // that will hold frame `total_frames_` next; the slot holding
        // frame `f` is therefore at offset (cap - (newest - f)) from
        // write_pos_ (equivalently, f mod cap aligned with write_pos_).
        const std::size_t back = static_cast<std::size_t>(newest_frame - f);
        const std::size_t slot = (write_pos_ + cap - back) % cap;
        dest[i] = ring_[slot];
    }
}

std::vector<std::int16_t>
LoopbackSink::aligned(std::chrono::steady_clock::time_point capture_time,
                       std::size_t samples_needed) const {
    std::vector<std::int16_t> out(samples_needed, std::int16_t{0});
    aligned(capture_time, std::span<std::int16_t>{out});
    return out;
}

std::uint64_t LoopbackSink::total_frames_emitted() const noexcept {
    std::lock_guard lk(mu_);
    return total_frames_;
}

bool LoopbackSink::has_data() const noexcept {
    std::lock_guard lk(mu_);
    return total_frames_ > 0;
}

void LoopbackSink::clear() noexcept {
    std::lock_guard lk(mu_);
    std::fill(ring_.begin(), ring_.end(), std::int16_t{0});
    write_pos_ = 0;
    total_frames_ = 0;
    last_emit_first_frame_ = 0;
    last_emit_time_ = std::chrono::steady_clock::time_point{};
}

} // namespace acva::audio
