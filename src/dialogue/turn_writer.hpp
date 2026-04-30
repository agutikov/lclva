#pragma once

#include "event/bus.hpp"
#include "event/event.hpp"
#include "memory/repository.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace acva::memory { class MemoryThread; }

namespace acva::dialogue {

// TurnWriter — persists user and assistant turns to SQLite via the
// memory thread.
//
// Subscribes to the bus on a single subscriber thread, so per-turn state
// in `in_flight_` is touched serially with no internal synchronization.
//
// User turns:
//   - Written on FinalTranscript with status=Committed.
//
//   - There is a small race with PromptBuilder: both the Manager (read)
//     and TurnWriter (write) react to the same FinalTranscript, and
//     whichever lands in the MemoryThread queue first wins. In the
//     unfavourable order, the prompt sees the just-written user turn in
//     `recent_turns` AND in `current_user_text` — duplicate. The model
//     handles it; M8 will tighten the ordering.
//
// Assistant turns:
//   - Tracked per Manager turn id (LlmStarted/LlmSentence/LlmFinished).
//   - LlmFinished{cancelled=false}: write status=Committed.
//   - LlmFinished{cancelled=true}  : write status=Interrupted iff at least
//                                    one sentence was emitted; otherwise
//                                    no row is written (Discarded).
//   - In M1 the "played-out portion" (project_design.md §6) is the
//     emitted-sentence text, since playback events don't fire yet. M3
//     will narrow this to the actually-played prefix using
//     PlaybackFinished.
class TurnWriter {
public:
    TurnWriter(event::EventBus& bus, memory::MemoryThread& memory);
    ~TurnWriter();

    TurnWriter(const TurnWriter&)            = delete;
    TurnWriter& operator=(const TurnWriter&) = delete;
    TurnWriter(TurnWriter&&)                 = delete;
    TurnWriter& operator=(TurnWriter&&)      = delete;

    void start();
    void stop();

    // Active session id used for every persisted row. main.cpp opens the
    // session once at startup and calls this before any FinalTranscript
    // can arrive. Defaults to 0 — writes are skipped at session 0 so
    // tests can verify the no-session-yet path safely.
    void set_session(memory::SessionId s) noexcept { session_.store(s, std::memory_order_release); }

private:
    void on_event(const event::Event& e);
    void handle_final(const event::FinalTranscript& e);
    void handle_started(const event::LlmStarted& e);
    void handle_sentence(const event::LlmSentence& e);
    void handle_finished(const event::LlmFinished& e);

    event::EventBus& bus_;
    memory::MemoryThread& memory_;
    event::SubscriptionHandle sub_;
    std::atomic<memory::SessionId> session_{0};

    struct AssistantState {
        std::string text;
        std::string lang;
        memory::UnixMs started_at = 0;
        std::uint32_t sentences = 0;
    };
    std::unordered_map<event::TurnId, AssistantState> in_flight_;
};

} // namespace acva::dialogue
