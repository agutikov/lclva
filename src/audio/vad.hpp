#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace acva::audio {

// Silero VAD wrapper around ONNX Runtime.
//
// Silero VAD operates on 16 kHz mono int16 audio in fixed 512-sample
// (32 ms) windows. The wrapper accepts arbitrary-size chunks via
// push_frame(): they accumulate in an internal scratch buffer until at
// least 512 samples are available, then the model runs and the new
// probability is cached. Subsequent push_frame() calls return the most
// recent probability until enough new audio has arrived to advance the
// window.
//
// Model file format: the official Silero VAD v5 ONNX model
// (silero_vad.onnx). Inputs are "input" [1, N], "state" [2, 1, 128],
// "sr" [1]. Outputs are "output" [1, 1] and "stateN" [2, 1, 128].
//
// Construction throws std::runtime_error if the model file is missing
// or fails to load.
class SileroVad {
public:
    explicit SileroVad(const std::filesystem::path& model_path,
                        std::uint32_t sample_rate = 16000);
    ~SileroVad();

    SileroVad(const SileroVad&)            = delete;
    SileroVad& operator=(const SileroVad&) = delete;
    SileroVad(SileroVad&&)                 noexcept;
    SileroVad& operator=(SileroVad&&)      noexcept;

    // Append `samples` to the internal scratch buffer. Whenever the
    // buffer has ≥ window_size_samples worth of audio, run the model
    // and cache the result. Returns the most recent probability of
    // speech in [0, 1].
    float push_frame(std::span<const std::int16_t> samples);

    // Reset hidden state between sessions.
    void reset();

    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::size_t   window_samples() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint32_t sample_rate_ = 16000;
};

} // namespace acva::audio
