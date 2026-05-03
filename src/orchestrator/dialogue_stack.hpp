#pragma once

#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/manager.hpp"
#include "dialogue/turn.hpp"
#include "dialogue/turn_writer.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "memory/memory_thread.hpp"
#include "memory/summarizer.hpp"
#include "metrics/registry.hpp"
#include "pipeline/fake_driver.hpp"
#include "supervisor/keep_alive.hpp"
#include "supervisor/supervisor.hpp"

#include <atomic>
#include <memory>
#include <variant>
#include <vector>

namespace acva::orchestrator {

// M1 + M2 + M5 dialogue stack: optional FakeDriver, plus the full
// LLM-driven path (prompt builder, LLM client, Manager, TurnWriter,
// Summarizer, KeepAlive) when cfg.llm.base_url is configured.
//
// build_dialogue_stack() always returns a non-null pointer;
// .has_llm() is false when cfg.llm.base_url is empty (the synthetic
// fake-driver-only path).
class DialogueStack {
public:
    DialogueStack() = default;
    ~DialogueStack();

    DialogueStack(const DialogueStack&)            = delete;
    DialogueStack& operator=(const DialogueStack&) = delete;
    DialogueStack(DialogueStack&&)                 = delete;
    DialogueStack& operator=(DialogueStack&&)      = delete;

    // Stop in the right order:
    //   1. fake_driver — no more synthetic events
    //   2. keep_alive — no more LLM keep-alive pings
    //   3. manager     — drain the in-flight LLM request
    //   4. turn_writer — flush pending memory writes
    //   5. summarizer  — drain the rolling summary worker
    // Idempotent.
    void stop();

    // Non-owning accessors. nullptr when LLM is disabled.
    [[nodiscard]] dialogue::Manager* manager() noexcept { return manager_.get(); }
    [[nodiscard]] bool               has_llm() const noexcept { return manager_ != nullptr; }

private:
    friend std::variant<std::unique_ptr<DialogueStack>, memory::DbError>
    build_dialogue_stack(
        const config::Config&,
        event::EventBus&,
        const std::shared_ptr<metrics::Registry>&,
        memory::MemoryThread&,
        dialogue::Fsm&,
        supervisor::Supervisor&,
        dialogue::TurnFactory&,
        const std::shared_ptr<std::atomic<event::TurnId>>&,
        std::vector<event::SubscriptionHandle>&);

    std::unique_ptr<pipeline::FakeDriver>         fake_driver_;
    std::unique_ptr<llm::PromptBuilder>           prompt_builder_;
    std::unique_ptr<llm::LlmClient>               llm_client_;
    std::unique_ptr<dialogue::Manager>            manager_;
    std::unique_ptr<dialogue::TurnWriter>         turn_writer_;
    std::unique_ptr<memory::Summarizer>           summarizer_;
    std::unique_ptr<supervisor::KeepAlive>        keep_alive_;

    bool stopped_ = false;
};

// Build the dialogue stack. Opens a fresh memory session before
// constructing the LLM-driven components. If the session insert fails,
// returns the DbError; callers report and exit.
//
// `playback_active_turn` is shared with the TTS stack — Manager
// updates it synchronously from its turn-started hook.
//
// LlmSentence stdout-echo subscription is appended to
// `subscription_keepalive` so it lives for the duration of the run.
[[nodiscard]] std::variant<std::unique_ptr<DialogueStack>, memory::DbError>
build_dialogue_stack(const config::Config& cfg,
                      event::EventBus& bus,
                      const std::shared_ptr<metrics::Registry>& registry,
                      memory::MemoryThread& memory,
                      dialogue::Fsm& fsm,
                      supervisor::Supervisor& sup,
                      dialogue::TurnFactory& turns,
                      const std::shared_ptr<std::atomic<event::TurnId>>& playback_active_turn,
                      std::vector<event::SubscriptionHandle>& subscription_keepalive);

} // namespace acva::orchestrator
