#include "dialogue/fsm.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>
#include <variant>

namespace lclva::dialogue {

std::string_view to_string(State s) noexcept {
    switch (s) {
        case State::Idle:         return "idle";
        case State::Listening:    return "listening";
        case State::UserSpeaking: return "user_speaking";
        case State::Transcribing: return "transcribing";
        case State::Thinking:     return "thinking";
        case State::Speaking:     return "speaking";
        case State::Completed:    return "completed";
        case State::Interrupted:  return "interrupted";
    }
    return "unknown";
}

std::string_view to_string(TurnOutcome o) noexcept {
    switch (o) {
        case TurnOutcome::NotStarted:  return "not_started";
        case TurnOutcome::Generating:  return "generating";
        case TurnOutcome::Speaking:    return "speaking";
        case TurnOutcome::Interrupted: return "interrupted";
        case TurnOutcome::Completed:   return "completed";
        case TurnOutcome::Discarded:   return "discarded";
    }
    return "unknown";
}

Fsm::Fsm(event::EventBus& bus, TurnFactory& turns) : bus_(bus), turns_(turns) {}

Fsm::~Fsm() {
    stop();
}

void Fsm::start() {
    if (sub_) {
        return; // already started
    }
    event::SubscribeOptions opts;
    opts.name = "dialogue.fsm";
    opts.queue_capacity = 512;
    opts.policy = event::OverflowPolicy::Block; // FSM must not lose control events
    sub_ = bus_.subscribe_all(std::move(opts), [this](const event::Event& e) {
        on_event(e);
    });

    transition(State::Listening, "started");
}

void Fsm::stop() {
    if (!sub_) {
        return;
    }
    sub_->stop();
    sub_.reset();
}

FsmSnapshot Fsm::snapshot() const {
    std::lock_guard lk(mu_);
    return FsmSnapshot{
        .state = state_,
        .active_turn = active_.id,
        .outcome = outcome_,
        .sentences_played = sentences_played_,
        .turns_completed = turns_completed_,
        .turns_interrupted = turns_interrupted_,
        .turns_discarded = turns_discarded_,
    };
}

void Fsm::transition(State next, std::string_view reason) {
    State prev;
    {
        std::lock_guard lk(mu_);
        prev = state_;
        state_ = next;
    }
    log::info("dialogue",
              fmt::format("fsm {} -> {} ({})", to_string(prev), to_string(next), reason));
}

void Fsm::on_event(const event::Event& evt) {
    std::visit([this]<class T>(const T& e) {
        if constexpr (std::is_same_v<T, event::SpeechStarted>)    handle_speech_started(e);
        else if constexpr (std::is_same_v<T, event::SpeechEnded>) handle_speech_ended(e);
        else if constexpr (std::is_same_v<T, event::FinalTranscript>) handle_final_transcript(e);
        else if constexpr (std::is_same_v<T, event::LlmSentence>) handle_llm_sentence(e);
        else if constexpr (std::is_same_v<T, event::LlmFinished>) handle_llm_finished(e);
        else if constexpr (std::is_same_v<T, event::PlaybackFinished>) handle_playback_finished(e);
        else if constexpr (std::is_same_v<T, event::UserInterrupted>) handle_user_interrupted(e);
        // Other events (PartialTranscript, LlmToken, TtsAudioChunk, etc.) are
        // observed but don't drive FSM transitions in M0.
    }, evt);
}

void Fsm::handle_speech_started(const event::SpeechStarted& /*e*/) {
    State current;
    {
        std::lock_guard lk(mu_);
        current = state_;
    }

    // From Listening / Completed / Interrupted: normal turn start.
    // From Speaking: this is barge-in — but barge-in arrives as
    // UserInterrupted, not as raw SpeechStarted. If we receive SpeechStarted
    // while Speaking it means VAD detected user speech without the
    // interruption-detection layer firing first; we still treat it as
    // beginning a new utterance.
    if (current == State::Listening || current == State::Completed
        || current == State::Interrupted) {
        TurnContext ctx = turns_.mint();
        {
            std::lock_guard lk(mu_);
            active_ = ctx;
            outcome_ = TurnOutcome::NotStarted;
            sentences_played_ = 0;
            sentences_received_ = 0;
            llm_finished_seen_ = false;
        }
        log::info("dialogue", fmt::format("turn {} started", ctx.id));
        transition(State::UserSpeaking, "speech_started");
    }
}

void Fsm::handle_speech_ended(const event::SpeechEnded& /*e*/) {
    State current;
    {
        std::lock_guard lk(mu_);
        current = state_;
    }
    if (current == State::UserSpeaking) {
        transition(State::Transcribing, "speech_ended");
    }
}

void Fsm::handle_final_transcript(const event::FinalTranscript& /*e*/) {
    State current;
    {
        std::lock_guard lk(mu_);
        current = state_;
        if (current == State::Transcribing) {
            outcome_ = TurnOutcome::Generating;
        }
    }
    if (current == State::Transcribing) {
        transition(State::Thinking, "final_transcript");
    }
}

void Fsm::handle_llm_sentence(const event::LlmSentence& /*e*/) {
    State current;
    {
        std::lock_guard lk(mu_);
        ++sentences_received_;
        current = state_;
        if (current == State::Thinking) {
            outcome_ = TurnOutcome::Speaking;
        }
    }
    if (current == State::Thinking) {
        transition(State::Speaking, "first_sentence");
    }
}

void Fsm::handle_llm_finished(const event::LlmFinished& /*e*/) {
    // The turn completes when (LlmFinished is seen) AND (every received
    // sentence has played). If LLM produced no sentences, complete immediately.
    bool transition_to_completed = false;
    {
        std::lock_guard lk(mu_);
        llm_finished_seen_ = true;
        if (state_ == State::Thinking && sentences_received_ == 0) {
            outcome_ = TurnOutcome::Completed;
            ++turns_completed_;
            transition_to_completed = true;
            if (outcome_observer_) outcome_observer_("completed");
        } else if (state_ == State::Speaking
                   && sentences_played_ == sentences_received_) {
            // Last sentence already played; LLM is the closer.
            outcome_ = TurnOutcome::Completed;
            ++turns_completed_;
            transition_to_completed = true;
            if (outcome_observer_) outcome_observer_("completed");
        }
    }
    if (transition_to_completed) {
        transition(State::Completed, "llm_finished");
        transition(State::Listening, "next_turn");
    }
}

void Fsm::handle_playback_finished(const event::PlaybackFinished& e) {
    bool turn_done = false;
    {
        std::lock_guard lk(mu_);
        // Reject stale playback events from previous turns (cancellation race).
        if (e.turn != active_.id) {
            return;
        }
        ++sentences_played_;
        // Only complete the turn once *both* the LLM has signalled it's done
        // and every emitted sentence has played. This handles the streaming
        // case where playback of sentence N finishes while the LLM is still
        // generating sentence N+1.
        if (state_ == State::Speaking
            && llm_finished_seen_
            && sentences_played_ == sentences_received_) {
            outcome_ = TurnOutcome::Completed;
            ++turns_completed_;
            turn_done = true;
        }
    }
    if (turn_done) {
        if (outcome_observer_) outcome_observer_("completed");
        transition(State::Completed, "playback_drained");
        transition(State::Listening, "next_turn");
    }
}

void Fsm::handle_user_interrupted(const event::UserInterrupted& e) {
    // Cancellation cascade per §6 of project_design.md.
    State prev_state;
    TurnContext old;
    bool already_played_a_sentence = false;
    {
        std::lock_guard lk(mu_);
        prev_state = state_;
        old = active_;
        already_played_a_sentence = sentences_played_ >= 1;
    }

    // Only meaningful while Speaking (or, defensively, in any active state).
    if (prev_state == State::Listening || prev_state == State::Idle) {
        return;
    }

    log::info("dialogue",
              fmt::format("user_interrupted: turn={} bumping; was_state={}",
                          e.turn, to_string(prev_state)));

    // ① Bump active turn id (invalidate the old token, mint a fresh context
    // for the next user utterance — but the next utterance triggers
    // SpeechStarted, which mints again. So here we just cancel.)
    if (old.token) {
        old.token->cancel();
    }
    // ② Cancel LLM stream — the LLM I/O layer observes the cancelled token
    // (M1 wiring). Until M1, this is a no-op besides bumping turn id.
    bus_.publish(event::CancelGeneration{ .turn = old.id });

    // Outcome decision: discarded vs interrupted.
    const char* outcome_label = nullptr;
    {
        std::lock_guard lk(mu_);
        if (already_played_a_sentence) {
            outcome_ = TurnOutcome::Interrupted;
            ++turns_interrupted_;
            outcome_label = "interrupted";
        } else {
            outcome_ = TurnOutcome::Discarded;
            ++turns_discarded_;
            outcome_label = "discarded";
        }
    }
    if (outcome_observer_ && outcome_label) {
        outcome_observer_(outcome_label);
    }
    transition(State::Interrupted, "user_interrupted");
    // ⑥ Resume listening; SpeechStarted (already published or about to be)
    // will mint a new turn context and transition to UserSpeaking.
    transition(State::Listening, "resume_listening");
}

} // namespace lclva::dialogue
