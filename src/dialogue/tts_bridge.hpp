#pragma once

#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/queue.hpp"
#include "tts/piper_client.hpp"   // re-uses TtsRequest + TtsCallbacks

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace acva::dialogue {

// TtsBridge — the adapter from the LLM-driven sentence stream to the
// playback queue.
//
// Subscribes:
//   • LlmStarted        — mints a per-turn CancellationToken.
//   • LlmSentence       — enqueues a synthesis job onto the I/O thread.
//   • LlmFinished       — drops the per-turn token (the sentences that
//                          made it onto the queue keep playing).
//   • UserInterrupted   — cancels the in-flight job, clears pending,
//                          drains the playback queue of stale audio.
//
// Threading:
//   • Bus subscriber thread (one) — handles every event above. Never
//     blocks on Piper or playback work.
//   • I/O thread (one) — pops the pending deque, calls
//     PiperClient::submit synchronously. submit() may block for
//     hundreds of ms per sentence; that is exactly why this thread
//     exists. Resampling happens inline inside the on_audio callback.
//
// Cancellation:
//   The bridge maintains its own per-turn token. On UserInterrupted it
//   flips the token and cancels the running submit() (which Piper sees
//   on the next chunk). The playback queue is also drained
//   immediately so the audio engine doesn't keep playing the already-
//   committed prefix.
//
// One job in flight at a time. Pending jobs are coalesced as a deque;
// production traffic is bounded by `cfg.dialogue.max_tts_queue_sentences`,
// which the LlmSentence path enforces upstream.
//
// Publishes TtsStarted / TtsAudioChunk / TtsFinished onto the bus
// for /metrics + observability.
class TtsBridge {
public:
    // Generic submit callable: matches the signature of both
    // PiperClient::submit and OpenAiTtsClient::submit. main.cpp picks
    // the right client based on cfg.tts.provider and binds its
    // submit method into this callback. Keeps the bridge decoupled
    // from any particular client class.
    using SubmitFn = std::function<void(tts::TtsRequest, tts::TtsCallbacks)>;

    TtsBridge(const config::Config& cfg,
               event::EventBus& bus,
               SubmitFn submit_fn,
               playback::PlaybackQueue& queue);
    ~TtsBridge();

    TtsBridge(const TtsBridge&)            = delete;
    TtsBridge& operator=(const TtsBridge&) = delete;
    TtsBridge(TtsBridge&&)                 = delete;
    TtsBridge& operator=(TtsBridge&&)      = delete;

    void start();
    void stop();

    // Counters for /metrics + tests.
    [[nodiscard]] std::uint64_t sentences_synthesized() const noexcept {
        return synthesized_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sentences_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t sentences_errored() const noexcept {
        return errored_.load(std::memory_order_relaxed);
    }
    // Pending jobs not yet pulled by the I/O thread. Read by tests; the
    // bridge enforces no upper bound itself — backpressure is the
    // dialogue.max_tts_queue_sentences cap upstream of the bridge.
    [[nodiscard]] std::size_t pending() const;

private:
    struct Job {
        event::LlmSentence sentence;
        std::shared_ptr<CancellationToken> cancel;
    };

    void on_event(const event::Event& e);
    void on_llm_started(event::TurnId turn);
    void on_llm_sentence(event::LlmSentence sentence);
    void on_llm_finished(const event::LlmFinished& e);
    void on_user_interrupted(const event::UserInterrupted& e);

    void io_loop();
    void run_one(Job job);

    [[nodiscard]] std::shared_ptr<CancellationToken> token_for(event::TurnId turn);

    const config::Config&    cfg_;
    event::EventBus&         bus_;
    SubmitFn                 submit_fn_;
    playback::PlaybackQueue& queue_;

    event::SubscriptionHandle sub_;
    std::thread io_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> pending_;
    bool stopping_ = false;

    // Per-turn token map. tokens_ is small (≤ 1 active turn at a time
    // in M3) — pruned on LlmFinished and on UserInterrupted.
    std::unordered_map<event::TurnId, std::shared_ptr<CancellationToken>> tokens_;

    // Set by run_one() while a submit() is in flight; checked by the
    // subscriber thread when UserInterrupted fires so the cancel
    // can route to the right token even if the sentence's own
    // turn-token has already been removed.
    std::shared_ptr<CancellationToken> in_flight_;

    std::atomic<std::uint64_t> synthesized_{0};
    std::atomic<std::uint64_t> cancelled_{0};
    std::atomic<std::uint64_t> errored_{0};
};

} // namespace acva::dialogue
