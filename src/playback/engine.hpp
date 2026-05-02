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

namespace acva::audio { class LoopbackSink; }

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
                    const config::PlaybackConfig& playback_cfg,
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

    // M6 — install the AEC reference tap. The audio thread calls
    // sink->on_emitted() on every chunk it hands to the device (or to
    // the headless ticker), with the wall-clock instant the chunk
    // begins playing. nullptr disables the tap. Must be called before
    // start() to avoid a brief race with the audio callback.
    void set_loopback_sink(audio::LoopbackSink* sink) noexcept { loopback_sink_ = sink; }

    // Counters (read by /metrics). All loads are relaxed.
    [[nodiscard]] std::uint64_t underruns()      const noexcept { return underruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t frames_played()  const noexcept { return frames_played_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t chunks_played()  const noexcept { return chunks_played_.load(std::memory_order_relaxed); }
    // Frames written as silence while waiting for the per-turn pre-buffer
    // threshold to be met. NOT counted as underruns — this is intentional
    // pre-buffer fill, not a starvation event.
    [[nodiscard]] std::uint64_t prefill_silence_frames() const noexcept {
        return prefill_silence_frames_.load(std::memory_order_relaxed);
    }
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

    const config::AudioConfig&    cfg_;
    const config::PlaybackConfig& playback_cfg_;
    PlaybackQueue&    queue_;
    event::EventBus&  bus_;
    ActiveTurnFn      active_turn_;

    std::size_t frames_per_buffer_ = 480;
    // Derived from playback_cfg_.prefill_ms × cfg_.sample_rate_hz at
    // construction time. 0 disables the pre-buffer altogether.
    std::size_t prefill_target_samples_ = 0;

    std::atomic<bool>          running_{false};
    std::atomic<bool>          headless_{false};
    std::atomic<bool>          force_headless_{false};
    std::chrono::milliseconds  headless_tick_{10};

    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> frames_played_{0};
    std::atomic<std::uint64_t> chunks_played_{0};
    std::atomic<std::uint64_t> prefill_silence_frames_{0};

    // Per-turn prefill state. Owned by the audio callback thread —
    // never read by anyone else, so no atomic needed. `primed_turn_`
    // is the active_turn for which the prefill threshold has already
    // been crossed; while active_turn != primed_turn_ we render
    // silence and wait for the queue to fill.
    dialogue::TurnId primed_turn_ = event::kNoTurn;

    // M6 — non-owning. nullptr disables the tap.
    audio::LoopbackSink* loopback_sink_ = nullptr;
};

} // namespace acva::playback
