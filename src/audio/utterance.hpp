#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace acva::audio {

// Reference-counted view over a single 16 kHz int16 utterance buffer.
// Multiple consumers (STT in M5, an optional disk recorder) hold
// shared_ptr<AudioSlice>; the storage is freed when the last reference
// drops.
//
// AudioSlice owns its sample storage; consumers should not mutate it
// (the immutability is contract, not enforced).
class AudioSlice {
public:
    AudioSlice(std::vector<std::int16_t> samples,
                std::uint32_t sample_rate,
                std::chrono::steady_clock::time_point started_at,
                std::chrono::steady_clock::time_point ended_at);

    [[nodiscard]] std::span<const std::int16_t> samples() const noexcept {
        return {samples_.data(), samples_.size()};
    }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept { return started_at_; }
    [[nodiscard]] std::chrono::steady_clock::time_point ended_at()   const noexcept { return ended_at_; }
    [[nodiscard]] std::chrono::milliseconds duration() const noexcept;

private:
    std::vector<std::int16_t> samples_;
    std::uint32_t              sample_rate_;
    std::chrono::steady_clock::time_point started_at_;
    std::chrono::steady_clock::time_point ended_at_;
};

// UtteranceBuffer assembles 16 kHz samples into AudioSlice instances
// driven by the Endpointer's SpeechStarted / SpeechEnded outcomes.
//
// Pre-padding model: even before SpeechStarted fires, the buffer
// maintains a rolling pre-buffer of the most recent `pre_padding_ms`
// of audio. When SpeechStarted lands, the rolling pre-buffer is
// adopted as the start of the current utterance — that catches the
// leading consonant the VAD missed.
//
// Post-padding: between Endpointer's hangover_ms detection and the
// emission of SpeechEnded, the buffer keeps appending; the trailing
// `post_padding_ms` are kept by `on_speech_ended` so the utterance
// doesn't get cut off at the exact endpoint frame.
//
// The buffer also enforces a max-in-flight cap: it counts how many
// utterances have been issued but not yet released by their consumers
// (i.e., still have non-zero shared_ptr refcount). When at the cap,
// new SpeechStarted is honored but the oldest in-flight slice is
// detached — the metric "voice_utterance_drops_total" should be bumped
// by the caller.
class UtteranceBuffer {
public:
    UtteranceBuffer(std::uint32_t sample_rate,
                     std::chrono::milliseconds pre_padding_ms,
                     std::chrono::milliseconds post_padding_ms,
                     std::size_t max_in_flight = 3,
                     std::chrono::milliseconds max_duration_ms = std::chrono::milliseconds{60000});

    // Always-on append path. Maintains the rolling pre-buffer when
    // not actively recording; when recording, appends to the
    // utterance's working buffer.
    void append(std::span<const std::int16_t> samples_16k);

    // Endpointer reported SpeechStarted. Adopts the rolling pre-buffer
    // as the prefix of the new utterance and starts active recording.
    // `pre_pad_start` is the steady_clock time matching the start of
    // the pre-buffered audio; `speech_started_at` matches the actual
    // VAD-detected start.
    void on_speech_started(std::chrono::steady_clock::time_point pre_pad_start,
                            std::chrono::steady_clock::time_point speech_started_at);

    // Endpointer reported SpeechEnded. Trims the trailing samples to
    // `post_padding_ms` past `speech_ended_at` and finalizes the slice.
    // Returns nullptr if no utterance was active.
    std::shared_ptr<AudioSlice> on_speech_ended(
        std::chrono::steady_clock::time_point speech_ended_at);

    // Drop the active recording without producing a slice. Used when
    // the endpointer reports a false start that survived past
    // SpeechStarted (i.e., max_duration_ms was exceeded).
    void abort_active();

    [[nodiscard]] std::size_t in_flight() const noexcept;
    [[nodiscard]] std::uint64_t drops() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] std::size_t pre_buffer_samples() const noexcept;

    // Snapshot of the rolling pre-buffer as a contiguous vector. Used
    // by the M5 streaming sink at SpeechStarted to replay the
    // pre-padding window so the realtime STT sees the leading
    // phoneme(s) the M4B request/response path keeps in the slice.
    // Cheap when the buffer is small (≤ 300 ms × 16 kHz × int16 ≈
    // 9.6 KB). Caller-thread only — same contract as append().
    [[nodiscard]] std::vector<std::int16_t> pre_buffer_snapshot() const;

    // Sample-rate-derived cap on the rolling pre-buffer (samples).
    [[nodiscard]] std::size_t pre_pad_capacity() const noexcept { return pre_pad_capacity_; }

private:
    void trim_pre_buffer();

    std::uint32_t              sample_rate_;
    std::chrono::milliseconds  pre_padding_ms_;
    std::chrono::milliseconds  post_padding_ms_;
    std::chrono::milliseconds  max_duration_ms_;
    std::size_t                max_in_flight_;
    std::size_t                pre_pad_capacity_;

    // Rolling pre-buffer (samples discarded oldest-first when full).
    std::deque<std::int16_t> pre_buffer_;

    // Active recording.
    bool                                  recording_ = false;
    std::vector<std::int16_t>             active_samples_;
    std::chrono::steady_clock::time_point active_started_at_{};
    std::chrono::steady_clock::time_point active_speech_started_at_{};

    // In-flight tracking. We keep weak_ptr to outstanding slices so
    // we can prune naturally when consumers drop their references.
    mutable std::mutex      mu_;
    std::deque<std::weak_ptr<AudioSlice>> in_flight_;
    std::uint64_t                          drops_ = 0;
};

} // namespace acva::audio
