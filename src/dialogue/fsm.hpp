#pragma once

#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>

namespace lclva::dialogue {

// Dialogue FSM states. Mirrors project_design.md §6.
//
// SpeculativeThinking from §6 is a concurrent sub-state used by streaming
// partial STT (M5). M0 implements the linear states only.
enum class State : std::uint8_t {
    Idle,
    Listening,
    UserSpeaking,
    Transcribing,
    Thinking,
    Speaking,
    Completed,
    Interrupted,
};

[[nodiscard]] std::string_view to_string(State s) noexcept;

// Assistant turn outcome (orthogonal to the FSM state). Tracks what to
// persist to memory once the turn ends.
enum class TurnOutcome : std::uint8_t {
    NotStarted,
    Generating,
    Speaking,
    Interrupted,
    Completed,
    Discarded,
};

[[nodiscard]] std::string_view to_string(TurnOutcome o) noexcept;

// Snapshot of FSM state for /status and tests. Captured under the FSM lock.
struct FsmSnapshot {
    State state = State::Idle;
    TurnId active_turn = kNoTurn;
    TurnOutcome outcome = TurnOutcome::NotStarted;
    std::uint32_t sentences_played = 0;
    std::uint64_t turns_completed = 0;
    std::uint64_t turns_interrupted = 0;
    std::uint64_t turns_discarded = 0;
};

// Dialogue FSM. Single-threaded by construction: subscribes to the bus with
// `subscribe_all`, so all events arrive on the subscription's worker thread
// and state transitions are serialized.
//
// On UserInterrupted, the FSM bumps the turn id (invalidating any in-flight
// work tagged with the old id) and cancels the previous turn's token. This
// is the cancellation cascade described in project_design.md §6.
class Fsm {
public:
    Fsm(event::EventBus& bus, TurnFactory& turns);
    ~Fsm();

    // Optional: called once per turn end with the outcome string
    // ("completed", "interrupted", "discarded"). Wired by main.cpp into
    // the metrics registry. Set before start().
    void set_turn_outcome_observer(std::function<void(const char*)> obs) {
        outcome_observer_ = std::move(obs);
    }

    Fsm(const Fsm&) = delete;
    Fsm& operator=(const Fsm&) = delete;
    Fsm(Fsm&&) = delete;
    Fsm& operator=(Fsm&&) = delete;

    // Begin handling events. Call once.
    void start();

    // Stop handling. Idempotent.
    void stop();

    [[nodiscard]] FsmSnapshot snapshot() const;

private:
    void on_event(const event::Event& evt);
    void handle_speech_started(const event::SpeechStarted& e);
    void handle_speech_ended(const event::SpeechEnded& e);
    void handle_final_transcript(const event::FinalTranscript& e);
    void handle_llm_finished(const event::LlmFinished& e);
    void handle_playback_finished(const event::PlaybackFinished& e);
    void handle_user_interrupted(const event::UserInterrupted& e);
    void handle_llm_sentence(const event::LlmSentence& e);

    void transition(State next, std::string_view reason);

    event::EventBus& bus_;
    TurnFactory& turns_;
    event::SubscriptionHandle sub_;

    mutable std::mutex mu_;
    State state_ = State::Idle;
    TurnContext active_;
    TurnOutcome outcome_ = TurnOutcome::NotStarted;
    std::uint32_t sentences_played_ = 0;
    std::uint32_t sentences_received_ = 0;
    bool llm_finished_seen_ = false;

    std::uint64_t turns_completed_ = 0;
    std::uint64_t turns_interrupted_ = 0;
    std::uint64_t turns_discarded_ = 0;

    std::function<void(const char*)> outcome_observer_;
};

} // namespace lclva::dialogue
