#include "audio/utterance.hpp"

#include <utility>

namespace acva::audio {

AudioSlice::AudioSlice(std::vector<std::int16_t> samples,
                        std::uint32_t sample_rate,
                        std::chrono::steady_clock::time_point started_at,
                        std::chrono::steady_clock::time_point ended_at)
    : samples_(std::move(samples)),
      sample_rate_(sample_rate),
      started_at_(started_at),
      ended_at_(ended_at) {}

std::chrono::milliseconds AudioSlice::duration() const noexcept {
    if (sample_rate_ == 0) return std::chrono::milliseconds{0};
    const std::uint64_t ms =
        static_cast<std::uint64_t>(samples_.size()) * 1000ULL
        / static_cast<std::uint64_t>(sample_rate_);
    return std::chrono::milliseconds{static_cast<std::int64_t>(ms)};
}

UtteranceBuffer::UtteranceBuffer(std::uint32_t sample_rate,
                                   std::chrono::milliseconds pre_padding_ms,
                                   std::chrono::milliseconds post_padding_ms,
                                   std::size_t max_in_flight,
                                   std::chrono::milliseconds max_duration_ms)
    : sample_rate_(sample_rate),
      pre_padding_ms_(pre_padding_ms),
      post_padding_ms_(post_padding_ms),
      max_duration_ms_(max_duration_ms),
      max_in_flight_(max_in_flight) {
    pre_pad_capacity_ =
        static_cast<std::size_t>(sample_rate_)
        * static_cast<std::size_t>(pre_padding_ms_.count())
        / 1000ULL;
}

void UtteranceBuffer::append(std::span<const std::int16_t> samples_16k) {
    if (samples_16k.empty()) return;
    if (recording_) {
        active_samples_.insert(active_samples_.end(),
                                samples_16k.begin(), samples_16k.end());
        // Hard cap: once the active buffer exceeds max_duration_ms,
        // truncate the head and reset the start timestamp so we never
        // keep more than the cap. Long monologues exceed this.
        const std::size_t cap_samples =
            static_cast<std::size_t>(sample_rate_)
            * static_cast<std::size_t>(max_duration_ms_.count())
            / 1000ULL;
        if (cap_samples > 0 && active_samples_.size() > cap_samples) {
            const std::size_t drop = active_samples_.size() - cap_samples;
            active_samples_.erase(active_samples_.begin(),
                                    active_samples_.begin()
                                        + static_cast<std::ptrdiff_t>(drop));
            active_started_at_ +=
                std::chrono::milliseconds{
                    static_cast<std::int64_t>(drop) * 1000
                    / static_cast<std::int64_t>(sample_rate_)};
        }
    } else {
        for (auto s : samples_16k) {
            pre_buffer_.push_back(s);
        }
        trim_pre_buffer();
    }
}

void UtteranceBuffer::trim_pre_buffer() {
    while (pre_buffer_.size() > pre_pad_capacity_) {
        pre_buffer_.pop_front();
    }
}

void UtteranceBuffer::on_speech_started(
    std::chrono::steady_clock::time_point pre_pad_start,
    std::chrono::steady_clock::time_point speech_started_at) {
    if (recording_) {
        // Already recording — caller didn't call on_speech_ended /
        // abort_active first. Drop the prior recording silently;
        // double-start is a programming error but recoverable.
        active_samples_.clear();
    }
    recording_                = true;
    active_speech_started_at_ = speech_started_at;
    active_started_at_        = pre_pad_start;
    active_samples_.clear();
    active_samples_.reserve(static_cast<std::size_t>(sample_rate_)
                              * 4); // ~4 s budget; grows as needed
    // Adopt the rolling pre-buffer as the leading prefix.
    active_samples_.assign(pre_buffer_.begin(), pre_buffer_.end());
    pre_buffer_.clear();
}

std::shared_ptr<AudioSlice> UtteranceBuffer::on_speech_ended(
    std::chrono::steady_clock::time_point speech_ended_at) {
    if (!recording_) return nullptr;

    // Trim the trailing samples to keep at most post_padding_ms past
    // the actual VAD endpoint. The active buffer has been growing
    // since SpeechStarted, so we can compute the trim from the start
    // timestamp + sample count.
    std::vector<std::int16_t> samples = std::move(active_samples_);
    active_samples_.clear();
    recording_ = false;

    // Compute target tail length: samples between
    // active_speech_started_at_ and speech_ended_at + post_padding.
    const auto active_dur =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            speech_ended_at - active_started_at_) + post_padding_ms_;
    const auto target_samples =
        static_cast<std::size_t>(sample_rate_)
        * static_cast<std::size_t>(active_dur.count())
        / 1000ULL;
    if (target_samples > 0 && samples.size() > target_samples) {
        samples.resize(target_samples);
    }

    auto slice = std::make_shared<AudioSlice>(
        std::move(samples), sample_rate_,
        active_started_at_, speech_ended_at);

    {
        std::lock_guard lk(mu_);
        // Prune dead weak_ptrs.
        for (auto it = in_flight_.begin(); it != in_flight_.end();) {
            if (it->expired()) it = in_flight_.erase(it);
            else ++it;
        }
        if (in_flight_.size() >= max_in_flight_) {
            // Drop the oldest by detaching its weak_ptr; if a consumer
            // still holds the shared_ptr, the slice survives — but our
            // tracker forgets it so it can't double-count.
            in_flight_.pop_front();
            ++drops_;
        }
        in_flight_.emplace_back(slice);
    }
    return slice;
}

void UtteranceBuffer::abort_active() {
    if (!recording_) return;
    active_samples_.clear();
    recording_ = false;
    active_started_at_        = {};
    active_speech_started_at_ = {};
}

std::size_t UtteranceBuffer::in_flight() const noexcept {
    std::lock_guard lk(mu_);
    std::size_t live = 0;
    for (const auto& w : in_flight_) {
        if (!w.expired()) ++live;
    }
    return live;
}

std::uint64_t UtteranceBuffer::drops() const noexcept {
    std::lock_guard lk(mu_);
    return drops_;
}

bool UtteranceBuffer::active() const noexcept { return recording_; }

std::size_t UtteranceBuffer::pre_buffer_samples() const noexcept {
    return pre_buffer_.size();
}

} // namespace acva::audio
