#pragma once

#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/endpointer.hpp"
#include "audio/resampler.hpp"
#include "audio/utterance.hpp"
#include "audio/vad.hpp"
#include "event/bus.hpp"

#include <string>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace acva::audio {

// Owns the consumer half of the M4 capture path: drains the SPSC ring
// (filled by CaptureEngine on PortAudio's callback thread), resamples
// 48 → 16 kHz, runs Silero VAD + Endpointer, assembles utterances,
// and publishes SpeechStarted / SpeechEnded / UtteranceReady on the
// event bus.
//
// One dedicated worker thread, never the audio callback thread. The
// resampler + VAD allocate; that's why they live here rather than in
// CaptureEngine.
class AudioPipeline {
public:
    struct Config {
        std::uint32_t            input_sample_rate = 48000;
        std::uint32_t            output_sample_rate = 16000;
        EndpointerConfig         endpointer{};
        std::chrono::milliseconds pre_padding_ms{300};
        std::chrono::milliseconds post_padding_ms{100};
        std::size_t              max_in_flight = 3;
        std::chrono::milliseconds max_duration_ms{60000};
        // Empty disables Silero — we still emit Speech* events
        // driven by a stub probability of 0 (i.e., never). The stub
        // is useful only as a placeholder; the loopback demo runs in
        // this mode for VAD-free testing.
        std::string vad_model_path;
    };

    AudioPipeline(Config cfg,
                   CaptureRing& ring,
                   MonotonicAudioClock& clock,
                   event::EventBus& bus);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&)            = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;
    AudioPipeline(AudioPipeline&&)                 = delete;
    AudioPipeline& operator=(AudioPipeline&&)      = delete;

    void start();
    void stop();

    [[nodiscard]] std::uint64_t frames_processed()  const noexcept { return frames_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t utterances_total()  const noexcept { return utterances_total_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t false_starts_total() const noexcept { return false_starts_total_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t utterance_drops()    const noexcept { return utterance_buffer_.drops(); }
    [[nodiscard]] std::size_t   in_flight()          const noexcept { return utterance_buffer_.in_flight(); }
    [[nodiscard]] bool          vad_enabled()        const noexcept { return vad_ != nullptr; }

    // Inspect the current endpointer state — useful for the capture demo.
    [[nodiscard]] EndpointerState endpointer_state() const noexcept { return endpointer_.state(); }

    // For tests: drain the ring N times synchronously without spawning
    // the worker thread. Returns the number of frames processed.
    std::size_t pump_for_test(std::size_t max_frames);

private:
    void run_loop();
    void process_frame(const AudioFrame& frame);

    Config           cfg_;
    CaptureRing&     ring_;
    MonotonicAudioClock& clock_;
    event::EventBus& bus_;

    Resampler        resampler_;
    std::unique_ptr<SileroVad> vad_;
    Endpointer       endpointer_;
    UtteranceBuffer  utterance_buffer_;

    std::thread       worker_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> frames_processed_{0};
    std::atomic<std::uint64_t> utterances_total_{0};
    std::atomic<std::uint64_t> false_starts_total_{0};

    // Cached VAD probability between Silero updates (it processes 32 ms
    // windows; we may step the endpointer at the resampled frame rate
    // ≈ 10 ms, so the same probability applies to multiple frames).
    float last_vad_p_ = 0.0F;
};

} // namespace acva::audio
