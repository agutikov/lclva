#pragma once

#include "audio/clock.hpp"
#include "audio/frame.hpp"
#include "audio/spsc_ring.hpp"
#include "config/config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace acva::audio {

// Capacity of the capture SPSC ring. 256 frames × 10 ms = 2.56 s of
// audio headroom — well past anything the consumer thread should ever
// fall behind on, but a hard upper bound on memory use (~2.5 MB at
// kMaxCaptureSamples).
inline constexpr std::size_t kCaptureRingCapacity = 256;

using CaptureRing = SpscRing<AudioFrame, kCaptureRingCapacity>;

// PortAudio input stream wrapper. The audio callback runs on PortAudio's
// realtime thread; it does exactly one thing per call: copy the
// incoming samples into a pre-allocated AudioFrame, push the frame onto
// the SPSC ring, and tick the MonotonicAudioClock. Never allocates,
// never logs, never touches the event bus.
//
// Two modes of operation, mirroring PlaybackEngine:
//   • Device mode  — Pa_OpenStream succeeds; audio reaches the ring at
//     real time.
//   • Headless mode — used when input_device == "none" or PortAudio
//     fails to open the device (CI containers without a card). The
//     ring stays empty; consumers can still be exercised via
//     CaptureEngine::inject_for_test in unit tests.
//
// The CaptureEngine does NOT spawn a consumer thread. main.cpp / the
// AudioPipeline owns that thread and pulls frames via try_pop().
class CaptureEngine {
public:
    CaptureEngine(const config::AudioConfig& audio_cfg,
                   CaptureRing& ring,
                   MonotonicAudioClock& clock);
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&)            = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;
    CaptureEngine(CaptureEngine&&)                 = delete;
    CaptureEngine& operator=(CaptureEngine&&)      = delete;

    // Open the device + start the input stream. Falls back to headless
    // when the device can't be opened. Returns true if either real or
    // headless capture is now running. Idempotent.
    bool start();

    // Stop the stream. Idempotent.
    void stop();

    // Force-headless override for tests. Must be called before start().
    void force_headless();

    // Push a synthetic frame onto the ring. Used by the unit tests and
    // by the loopback / capture demos when running in headless mode.
    // Increments the clock as if a real callback fired.
    bool inject_for_test(std::span<const std::int16_t> samples);

    [[nodiscard]] bool running()   const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] bool headless()  const noexcept { return headless_.load(std::memory_order_acquire); }

    // Counters (read by /metrics + demos).
    [[nodiscard]] std::uint64_t frames_captured()  const noexcept { return frames_captured_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t ring_overruns()    const noexcept { return ring_overruns_.load(std::memory_order_relaxed); }

    // Audio-thread entry point — copies `frame_count` int16 mono
    // samples from `input` into a fresh AudioFrame and pushes to the
    // ring. Public because the PortAudio callback trampoline (defined
    // in capture.cpp) calls it. Tests call it indirectly via
    // inject_for_test.
    void on_input(const std::int16_t* input, std::size_t frame_count) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    const config::AudioConfig& cfg_;
    CaptureRing&         ring_;
    MonotonicAudioClock& clock_;

    std::atomic<bool> running_{false};
    std::atomic<bool> headless_{false};
    std::atomic<bool> force_headless_{false};

    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> ring_overruns_{0};
};

} // namespace acva::audio
