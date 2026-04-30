#pragma once

#include "event/bus.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace acva::pipeline {

struct FakeDriverOptions {
    // Inter-turn pause.
    std::chrono::milliseconds idle_between_turns{1500};
    // Per-turn timing (rough sketch of a real turn).
    std::chrono::milliseconds user_speech_duration{800};
    std::chrono::milliseconds stt_processing{300};
    std::chrono::milliseconds llm_first_token_delay{400};
    std::chrono::milliseconds llm_per_sentence{600};
    std::chrono::milliseconds tts_first_audio{200};
    std::chrono::milliseconds playback_per_sentence{900};
    // How many sentences the LLM "produces" per turn.
    std::uint32_t sentences_per_turn = 3;
    // Probability (0..1) of barge-in mid-turn. 0 = no barge-in.
    double barge_in_probability = 0.0;
    // Random seed for reproducibility. 0 = use steady_clock.
    std::uint64_t seed = 0;
};

// Synthetic event generator used in M0 to drive the FSM end-to-end before the
// real audio / STT / LLM / TTS layers exist. Publishes events to the bus on
// its own thread; stop() halts the loop.
class FakeDriver {
public:
    FakeDriver(event::EventBus& bus, FakeDriverOptions opts);
    ~FakeDriver();

    FakeDriver(const FakeDriver&) = delete;
    FakeDriver& operator=(const FakeDriver&) = delete;
    FakeDriver(FakeDriver&&) = delete;
    FakeDriver& operator=(FakeDriver&&) = delete;

    // Run forever (or until stop()). Each iteration: emit one synthetic turn.
    void start();
    void stop();

    [[nodiscard]] std::uint64_t turns_emitted() const noexcept {
        return turns_emitted_.load(std::memory_order_relaxed);
    }

private:
    void run_loop();
    void run_one_turn(event::TurnId turn);
    void run_one_turn_with_barge_in(event::TurnId turn);

    event::EventBus& bus_;
    FakeDriverOptions opts_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> turns_emitted_{0};
};

} // namespace acva::pipeline
