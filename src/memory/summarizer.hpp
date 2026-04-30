#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "memory/repository.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace lclva::llm { class LlmClient; }
namespace lclva::memory { class MemoryThread; }

namespace lclva::memory {

// Summarizer — keeps a rolling summary of the conversation in the
// `summaries` table.
//
// Subscribes to LlmFinished on the bus. On each completed assistant turn
// the in-memory turn counter advances; once it crosses
// cfg.memory.summary.turn_threshold a worker thread runs an LLM call
// that produces a 1–2-sentence summary and writes it via the memory
// thread.
//
// Single in-flight summary at a time. Subsequent triggers that arrive
// while a summary is running are coalesced (boolean job_pending_); the
// worker re-checks the threshold after each run.
//
// Per plans/milestones/m1_llm_memory.md C.2.1, M1 lands the machinery
// only — prompt iteration is M8.
class Summarizer {
public:
    Summarizer(const config::Config& cfg,
               event::EventBus& bus,
               MemoryThread& memory,
               llm::LlmClient& client);
    ~Summarizer();

    Summarizer(const Summarizer&)            = delete;
    Summarizer& operator=(const Summarizer&) = delete;
    Summarizer(Summarizer&&)                 = delete;
    Summarizer& operator=(Summarizer&&)      = delete;

    void start();
    void stop();

    void set_session(SessionId s) noexcept { session_.store(s, std::memory_order_release); }

    [[nodiscard]] std::uint64_t summaries_written() const noexcept {
        return summaries_written_.load(std::memory_order_acquire);
    }

    // Force a summarization run regardless of trigger threshold. Returns
    // immediately; the summary lands asynchronously.
    void trigger_now();

private:
    void on_event(const event::Event& e);
    void worker_loop();
    bool run_one_summary();   // returns true if a row was written

    const config::Config& cfg_;
    event::EventBus& bus_;
    MemoryThread& memory_;
    llm::LlmClient& client_;

    event::SubscriptionHandle sub_;
    std::atomic<SessionId> session_{0};
    std::atomic<std::size_t> turns_since_summary_{0};
    std::atomic<std::uint64_t> summaries_written_{0};

    std::thread worker_;
    std::mutex worker_mu_;
    std::condition_variable worker_cv_;
    bool job_pending_ = false;
    bool stopping_ = false;
};

} // namespace lclva::memory
