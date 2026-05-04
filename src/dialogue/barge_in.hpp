#pragma once

#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace acva::audio { class Apm; }

namespace acva::dialogue {

// M7 — barge-in detector. Promotes a VAD `SpeechStarted` that arrives
// while the assistant is in `Speaking` state into a `UserInterrupted`
// event, kicking off the cancellation cascade defined in §6 of
// project_design.md.
//
// Subscribes to `SpeechStarted` on the bus. On each, it:
//   1. Snapshots the FSM. If state != Speaking, ignore.
//   2. Checks the cool-down: if `now - entered_speaking_at` is shorter
//      than `cool_down_after_turn_ms`, suppress (avoids firing on the
//      user's own residual breath / lip noise + the AEC convergence
//      transient at the start of TTS).
//   3. Checks AEC convergence (when `apm` is non-null and
//      `require_aec_converged` is set). Skipped on headphone-only setups
//      where there's no echo path to converge against.
//   4. Publishes `UserInterrupted{turn = active_turn}`.
//
// Threading: subscribes with an event-bus subscription (its own worker
// thread). One instance per orchestrator. No lock — `last_fired_turn_`
// is atomic; everything else is read/written from the subscriber thread.
class BargeInDetector {
public:
    BargeInDetector(event::EventBus& bus,
                    const Fsm& fsm,
                    const audio::Apm* apm,
                    const config::BargeInConfig& cfg);
    ~BargeInDetector();

    BargeInDetector(const BargeInDetector&)            = delete;
    BargeInDetector& operator=(const BargeInDetector&) = delete;
    BargeInDetector(BargeInDetector&&)                 = delete;
    BargeInDetector& operator=(BargeInDetector&&)      = delete;

    // Subscribe to the bus. No-op when cfg.enabled == false. Idempotent.
    void start();

    // Stop the subscription. Idempotent.
    void stop();

    // Fired AFTER each UserInterrupted publish, on the detector's
    // subscription thread, with the cancelled turn id and the publish
    // timestamp. Wired in main.cpp to PlaybackEngine::note_barge_in
    // so the engine can record the barge-in latency. Set before
    // start(); the callback may be called concurrently with stop()
    // from the subscription thread.
    using OnFiredFn = std::function<void(event::TurnId,
                                          std::chrono::steady_clock::time_point)>;
    void set_on_fired(OnFiredFn cb) { on_fired_ = std::move(cb); }

    // Counters mirrored on /metrics. All loads relaxed.
    [[nodiscard]] std::uint64_t fires_total() const noexcept {
        return fires_total_.load(std::memory_order_relaxed);
    }
    // Number of SpeechStarted events the detector saw while in
    // Speaking but suppressed for one of: cooldown, AEC not active,
    // ERLE below threshold. Useful for tuning the gates without
    // injecting fake events.
    [[nodiscard]] std::uint64_t suppressed_total() const noexcept {
        return suppressed_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t suppressed_cooldown() const noexcept {
        return suppressed_cooldown_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t suppressed_aec() const noexcept {
        return suppressed_aec_.load(std::memory_order_relaxed);
    }

    // Time of the most recent UserInterrupted publish, or
    // default-constructed if none. Used by the latency metric (M7 §4):
    // the playback engine reads this and computes the delta to the
    // first zero-out audio buffer.
    [[nodiscard]] std::chrono::steady_clock::time_point last_fired_at() const noexcept;

    // Test hook — synchronously evaluate the gates for a SpeechStarted
    // event and fire if they pass. Bypasses the bus subscription so
    // unit tests don't have to spin up a worker thread. Returns true
    // when UserInterrupted was published.
    bool evaluate_for_test(const event::SpeechStarted& e);

private:
    void on_speech_started(const event::SpeechStarted& e);

    // Returns true when AEC requirements are satisfied (or not
    // required by config). Non-const because it bumps the per-cause
    // suppression counter on a miss.
    bool aec_ok();

    event::EventBus&             bus_;
    const Fsm&                   fsm_;
    const audio::Apm*            apm_;     // nullable
    config::BargeInConfig        cfg_;

    event::SubscriptionHandle    sub_;

    std::atomic<std::uint64_t>   fires_total_{0};
    std::atomic<std::uint64_t>   suppressed_total_{0};
    std::atomic<std::uint64_t>   suppressed_cooldown_{0};
    std::atomic<std::uint64_t>   suppressed_aec_{0};

    // Stored as nanoseconds since the steady_clock epoch so we can
    // load atomically. 0 == never fired.
    std::atomic<std::int64_t>    last_fired_ns_{0};

    // Most recently observed active_turn that *we* have already fired
    // a UserInterrupted for. Prevents firing twice for the same turn
    // when VAD reports rapid consecutive SpeechStarted events.
    std::atomic<event::TurnId>   last_fired_turn_{event::kNoTurn};

    OnFiredFn                    on_fired_;
};

} // namespace acva::dialogue
