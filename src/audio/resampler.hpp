#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Forward-declare the soxr handle so users of this header don't need
// soxr.h. The .cpp owns the include.
struct soxr;

namespace acva::audio {

// Streaming int16 mono resampler built on soxr.
//
// Designed for use in the M3 TTS path (22'050 Hz Piper output →
// 48 kHz playback device) and the M4 capture path (mic device rate
// → 16 kHz APM/VAD pipeline). The same Resampler instance can be
// fed an arbitrary stream of input chunks via process(); flush()
// drains soxr's internal buffers when the stream ends.
//
// Phase-stable across chunk boundaries — soxr maintains state
// internally, so calling process() repeatedly is equivalent to
// resampling the concatenated input once. This matters for AEC
// reference-signal alignment in M6: the same library and the same
// quality setting are used everywhere so phase doesn't drift across
// the loopback tap.
//
// Not threadsafe. Each consumer owns its own Resampler. Construction
// allocates; process()/flush() never reallocate the soxr handle but
// do return a freshly-allocated std::vector each call (the audio
// thread must not call this directly — schedule resampling on the
// playback bookkeeping thread).
class Resampler {
public:
    enum class Quality : std::uint8_t {
        Quick,        // SOXR_QQ — fastest, audible aliasing
        Low,          // SOXR_LQ
        Medium,       // SOXR_MQ
        High,         // SOXR_HQ — default; phase-stable, transparent for speech
        VeryHigh,     // SOXR_VHQ — slowest, mastering-grade
    };

    // Construct a resampler that converts `in_rate` → `out_rate` (Hz).
    // Throws std::runtime_error if soxr fails to initialize (rates ≤ 0
    // or unsupported ratio).
    Resampler(double in_rate, double out_rate, Quality quality = Quality::High);
    ~Resampler();

    Resampler(const Resampler&)            = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&)                 noexcept;
    Resampler& operator=(Resampler&&)      noexcept;

    // Convert one chunk of input. Returns the resampled output. May be
    // shorter or longer than the input depending on the rate ratio. An
    // empty input returns an empty output without disturbing soxr's
    // internal state.
    [[nodiscard]] std::vector<std::int16_t> process(std::span<const std::int16_t> in);

    // Drain soxr's internal buffer. Call once at end-of-stream.
    // Resamplers are not reusable across streams — construct a new
    // one for the next utterance.
    [[nodiscard]] std::vector<std::int16_t> flush();

    [[nodiscard]] double in_rate()  const noexcept { return in_rate_; }
    [[nodiscard]] double out_rate() const noexcept { return out_rate_; }
    [[nodiscard]] Quality quality() const noexcept { return quality_; }

    // Total input/output sample counts since construction. Useful for
    // tests and the playback queue's "is this drained?" check.
    [[nodiscard]] std::uint64_t input_samples()  const noexcept { return in_samples_; }
    [[nodiscard]] std::uint64_t output_samples() const noexcept { return out_samples_; }

private:
    soxr*    soxr_       = nullptr;
    double   in_rate_    = 0.0;
    double   out_rate_   = 0.0;
    Quality  quality_    = Quality::High;
    std::uint64_t in_samples_  = 0;
    std::uint64_t out_samples_ = 0;
};

} // namespace acva::audio
