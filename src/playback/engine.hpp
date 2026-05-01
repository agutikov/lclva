#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "event/queue.hpp"
#include "playback/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace acva::playback {

// Drives a real PortAudio output stream from the PlaybackQueue, or a
// "headless" timer-driven loop when no device is available.
//
// Two modes, one code path:
//
//   • Device mode  — PortAudio opens the output stream named in
//     cfg.audio.output_device ("default" picks the host default).
//     The realtime callback drains the queue, fills the device
//     buffer, and posts PlaybackFinished events via an SPSC-style
//     postbox. The callback never blocks, never allocates, and never
//     touches the event bus directly.
//
//   • Headless mode — used when output_device == "none" or when
//     PortAudio fails to open (CI containers, soak harness without an
//     audio card). A regular thread runs the same chunk-consumer
//     state machine on a timer; everything downstream behaves
//     identically. Tests rely on this to exercise the engine.
//
// Threading:
//   • Producer (TtsBridge) → enqueue() into PlaybackQueue.
//   • Audio thread (PortAudio cb or headless ticker) → dequeue and
//     mix into the output buffer; pushes PlaybackFinished into the
//     postbox.
//   • Publisher thread → pops from the postbox and calls
//     bus.publish(). Owns the event-bus interaction so the audio
//     thread never blocks on the bus's mutex.
class PlaybackEngine {
public:
    // Source of the currently-active dialogue turn. The audio thread
    // calls this lock-free on every chunk pop. The default in main.cpp
    // forwards to fsm.snapshot().active_turn; tests inject a stub.
    using ActiveTurnFn = std::function<dialogue::TurnId()>;

    PlaybackEngine(const config::AudioConfig& audio_cfg,
                    PlaybackQueue& queue,
                    event::EventBus& bus,
                    ActiveTurnFn active_turn);
    ~PlaybackEngine();

    PlaybackEngine(const PlaybackEngine&)            = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&)                 = delete;
    PlaybackEngine& operator=(PlaybackEngine&&)      = delete;

    // Open the device + start the stream. Falls back to headless when
    // PortAudio init fails or output_device == "none". Returns true if
    // either real or headless playback is now running. Idempotent —
    // a second call is a no-op.
    bool start();

    // Stop the stream + drain the publisher. Idempotent.
    void stop();

    // Force-headless override for tests. Must be called before start().
    // Sets the headless tick interval.
    void force_headless(std::chrono::milliseconds tick = std::chrono::milliseconds(10));

    // Counters (read by /metrics). All loads are relaxed.
    [[nodiscard]] std::uint64_t underruns()      const noexcept { return underruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_played()  const noexcept { return frames_played_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t chunks_played()  const noexcept { return chunks_played_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool running() const noexcept                  { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] bool headless() const noexcept                 { return headless_.load(std::memory_order_acquire); }

    // Number of frames pulled from the queue per audio "tick"; mirrors
    // cfg.audio.buffer_frames in device mode and is identical in
    // headless mode (so soak metrics match real runs).
    [[nodiscard]] std::size_t frames_per_buffer() const noexcept { return frames_per_buffer_; }

    // Audio-thread entry point — fills `frames` int16 mono samples
    // into `out`, draining from the PlaybackQueue. Realtime-safe:
    // never allocates and only blocks on the queue's brief mutex.
    // Public because the PortAudio callback trampoline (defined in
    // engine.cpp with PortAudio's typed signature) calls it; tests
    // can also invoke it to drive the engine without an audio device.
    void render_into(std::int16_t* out, std::size_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    const config::AudioConfig& cfg_;
    PlaybackQueue&    queue_;
    event::EventBus&  bus_;
    ActiveTurnFn      active_turn_;

    std::size_t frames_per_buffer_ = 480;

    std::atomic<bool>          running_{false};
    std::atomic<bool>          headless_{false};
    std::atomic<bool>          force_headless_{false};
    std::chrono::milliseconds  headless_tick_{10};

    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> frames_played_{0};
    std::atomic<std::uint64_t> chunks_played_{0};
};

} // namespace acva::playback
