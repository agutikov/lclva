#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace acva::supervisor {

// KeepAlive — periodic timer that pings the LLM while the dialogue is
// idle, so llama.cpp doesn't offload the model.
//
// Every `interval` ticks, the loop checks the `should_fire` predicate.
// When it returns true, `on_tick` is invoked (typically calls
// `LlmClient::keep_alive()`). When false, the tick is skipped — the
// counter still increments via `on_skipped` so /metrics can show how
// often the timer was suppressed by an active turn.
//
// Both callbacks run on the keep-alive thread. on_tick may block (the
// llama.cpp /v1/chat/completions call is synchronous); the next tick
// is scheduled `interval` ms after on_tick returns, so a slow ping
// won't queue up behind itself.
//
// Cancellation: stop() flips an atomic and wakes the cv; the loop
// returns within at most one cv predicate check.
class KeepAlive {
public:
    using FireCheck = std::function<bool()>;
    using Tick      = std::function<void()>;
    using Counter   = std::function<void()>;   // optional metric hook

    struct Options {
        std::chrono::milliseconds interval{60'000};
        FireCheck   should_fire;     // required; true → fire this tick
        Tick        on_tick;         // required; the work to do
        Counter     on_fired;        // optional; called after on_tick returns
        Counter     on_skipped;      // optional; called when should_fire was false
    };

    explicit KeepAlive(Options opts);
    ~KeepAlive();

    KeepAlive(const KeepAlive&)            = delete;
    KeepAlive& operator=(const KeepAlive&) = delete;
    KeepAlive(KeepAlive&&)                 = delete;
    KeepAlive& operator=(KeepAlive&&)      = delete;

    // Begin ticking. Idempotent. The first tick fires after `interval`,
    // not immediately — keeps boot latency clean.
    void start();

    // Stop ticking. Idempotent. Drains the timer thread.
    void stop();

    // Counters readable from any thread (tests + /metrics).
    [[nodiscard]] std::uint64_t fired() const noexcept   { return fired_.load(); }
    [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_.load(); }

private:
    void run();

    Options opts_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> fired_{0};
    std::atomic<std::uint64_t> skipped_{0};

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
};

} // namespace acva::supervisor
