#pragma once

#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "memory/repository.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace acva::llm {
class LlmClient;
class PromptBuilder;
} // namespace acva::llm

namespace acva::dialogue {

// DialogueManager — wires FinalTranscript → LLM → LlmSentence events.
//
// Two threads, intentionally:
//   - Bus subscriber thread: handles FinalTranscript / CancelGeneration /
//     UserInterrupted. Never blocks on I/O. FinalTranscript is enqueued
//     to the I/O thread; cancel events flip the active token directly.
//   - I/O thread: runs PromptBuilder::build() and LlmClient::submit().
//     submit() blocks here for the duration of the SSE stream so its
//     callbacks (on_token / on_finished) can synchronously publish
//     LlmSentence / drive the SentenceSplitter without touching the
//     subscriber thread.
//
// Why two threads: if submit() ran on the subscriber thread, CancelGen
// events would queue up behind it and only fire after the stream
// completed — defeating cancellation entirely.
//
// M1 keeps the I/O thread serial: a single in-flight slot, one stream
// at a time. M5 (speculative LLM) introduces a second slot.
//
// M1 mints its own TurnContext per FinalTranscript. When M3+ wires the
// FSM properly (FSM.active_ becomes the source of truth from
// SpeechStarted onwards), this can switch to reading the FSM's context.
class Manager {
public:
    Manager(const config::Config& cfg,
            event::EventBus& bus,
            llm::PromptBuilder& prompt_builder,
            llm::LlmClient& client,
            TurnFactory& turns);
    ~Manager();

    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&)                 = delete;
    Manager& operator=(Manager&&)      = delete;

    void start();
    void stop();

    // The session id PromptBuilder uses for facts/summary/recent-turns
    // lookups. main.cpp sets this once per process run after opening the
    // session in SQLite. Defaults to 0 (cold start; no memory included).
    void set_session(memory::SessionId s) noexcept { session_.store(s, std::memory_order_release); }

private:
    void on_event(const event::Event& e);
    void enqueue_turn(event::FinalTranscript e);
    void cancel_active(event::TurnId target);
    void io_loop();
    void run_one(const event::FinalTranscript& e);

    const config::Config& cfg_;
    event::EventBus& bus_;
    llm::PromptBuilder& prompt_builder_;
    llm::LlmClient& client_;
    TurnFactory& turns_;

    event::SubscriptionHandle sub_;
    std::atomic<memory::SessionId> session_{0};

    // I/O thread machinery — one in-flight slot.
    std::thread io_;
    std::mutex io_mu_;
    std::condition_variable io_cv_;
    std::optional<event::FinalTranscript> pending_;
    bool stopping_ = false;

    // Active turn for the *currently running* I/O job. Read by
    // cancel_active() from the subscriber thread; written by the I/O
    // thread when starting / finishing a job.
    std::mutex active_mu_;
    TurnContext active_;
};

} // namespace acva::dialogue
