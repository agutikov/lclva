#pragma once

#include <atomic>
#include <chrono>

namespace acva::audio {

// Half-duplex mic gate for the speakers-without-AEC fallback.
//
// When `cfg.audio.half_duplex_while_speaking` is true, main.cpp
// constructs one of these and wires:
//   • Fsm::set_state_observer  → set_speaking(true/false)
//   • CaptureEngine::on_input  → samples dropped while
//                                 should_drop_now() is true
//
// During the assistant's `Speaking` state the gate is "speaking-on" and
// the capture path silently drops mic samples before they enter the
// SPSC ring; Silero VAD therefore never sees the assistant's own
// voice and can't fire phantom `SpeechStarted` events. After the
// FSM leaves `Speaking`, the gate keeps dropping for an additional
// `hangover` window to absorb speaker tail / room reverb / amplifier
// decay.
//
// **Trade-off:** no barge-in. The user can't interrupt the assistant
// because the mic isn't being listened to. This is the explicit
// alternative to M6 AEC — see plans/milestones/m5_streaming_stt.md
// "Half-duplex" section. Defaults off.
//
// **Threading:** lock-free. `set_speaking` is called from the
// dialogue/FSM thread; `should_drop_now` is called from PortAudio's
// realtime callback thread. Both are atomic loads/stores; no allocation
// in the hot path.
//
// `Clock` is a template parameter to allow injecting a virtual clock
// in unit tests. Default is `std::chrono::steady_clock`.

template <class Clock = std::chrono::steady_clock>
class BasicHalfDuplexGate {
public:
    explicit BasicHalfDuplexGate(std::chrono::milliseconds hangover) noexcept
        : hangover_(hangover) {}

    BasicHalfDuplexGate(const BasicHalfDuplexGate&)            = delete;
    BasicHalfDuplexGate& operator=(const BasicHalfDuplexGate&) = delete;

    // Toggle speaking state. Anchors the hangover timer on the
    // falling edge (speaking → not speaking); cancels any active
    // hangover on the rising edge so a back-to-back sentence
    // doesn't re-arm the gate against itself. Idempotent — repeated
    // identical calls do not move the anchor.
    void set_speaking(bool active) noexcept {
        const bool prev = speaking_.exchange(active, std::memory_order_acq_rel);
        if (prev && !active) {
            // Falling edge: anchor the hangover.
            ended_at_.store(now_rep(), std::memory_order_release);
            in_hangover_.store(true, std::memory_order_release);
        } else if (!prev && active) {
            // Rising edge (or first activation): cancel any pending
            // hangover from a prior session.
            in_hangover_.store(false, std::memory_order_release);
        }
        // No-change edges (true→true, false→false) intentionally
        // leave the timer state untouched.
    }

    // Realtime-safe predicate. Returns true if the capture path
    // should currently drop mic samples.
    [[nodiscard]] bool should_drop_now() const noexcept {
        if (speaking_.load(std::memory_order_acquire)) return true;
        if (!in_hangover_.load(std::memory_order_acquire)) return false;
        const auto since = now_rep() - ended_at_.load(std::memory_order_acquire);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   typename Clock::duration{since})
            < hangover_;
    }

    [[nodiscard]] bool speaking() const noexcept {
        return speaking_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::chrono::milliseconds hangover() const noexcept {
        return hangover_;
    }

private:
    static typename Clock::rep now_rep() noexcept {
        return Clock::now().time_since_epoch().count();
    }

    std::atomic<bool>                     speaking_{false};
    std::atomic<bool>                     in_hangover_{false};
    std::atomic<typename Clock::rep>      ended_at_{0};
    const std::chrono::milliseconds       hangover_;
};

using HalfDuplexGate = BasicHalfDuplexGate<>;

} // namespace acva::audio
