#pragma once

#include "audio/loopback.hpp"
#include "config/config.hpp"
#include "dialogue/tts_bridge.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "metrics/registry.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "tts/openai_tts_client.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>

namespace acva::orchestrator {

// M3 + M4B + M6 TTS stack. Holds the entire playback path along with
// the optional self-listen feedback loop. Lifetime ordering on
// teardown is non-trivial (bridge → self-listen → engine → metrics
// poller), so this RAII bundle owns everything and provides a single
// stop() that runs the right sequence.
//
// build_tts_stack() returns a non-null pointer ALWAYS — even when
// cfg.tts.voices is empty (the "disabled" case). The disabled bundle
// has every member nulled out and stop() is a no-op; main.cpp
// branches on `bridge != nullptr` to decide whether dialogue can
// drive playback.
class TtsStack {
public:
    TtsStack() = default;
    ~TtsStack();

    TtsStack(const TtsStack&)            = delete;
    TtsStack& operator=(const TtsStack&) = delete;
    TtsStack(TtsStack&&)                 = delete;
    TtsStack& operator=(TtsStack&&)      = delete;

    // Stop in the right order:
    //   1. tts_bridge.stop() — no new TTS requests
    //   2. self-listen worker join (it pulls from a queue the bridge feeds)
    //   3. playback_engine.stop() — drains the audio cb thread
    //   4. metrics poller join — last so it sees a clean final state
    // Idempotent.
    void stop();

    // Non-owning accessors for the rest of the orchestrator. nullptr
    // when TTS is disabled (cfg.tts.voices empty).
    [[nodiscard]] dialogue::TtsBridge*  bridge()       noexcept { return bridge_.get(); }
    [[nodiscard]] playback::PlaybackEngine* engine()   noexcept { return engine_.get(); }
    [[nodiscard]] audio::LoopbackSink*  loopback()     noexcept { return loopback_.get(); }
    [[nodiscard]] bool                  enabled() const noexcept { return bridge_ != nullptr; }

private:
    friend std::unique_ptr<TtsStack> build_tts_stack(
        const config::Config&,
        event::EventBus&,
        const std::shared_ptr<metrics::Registry>&,
        const std::shared_ptr<std::atomic<event::TurnId>>&);

    std::unique_ptr<playback::PlaybackQueue>  queue_;
    std::unique_ptr<tts::OpenAiTtsClient>     client_;
    std::unique_ptr<playback::PlaybackEngine> engine_;
    std::unique_ptr<audio::LoopbackSink>      loopback_;
    std::unique_ptr<dialogue::TtsBridge>      bridge_;

    std::thread       metrics_thread_;
    std::atomic<bool> metrics_stop_{false};

    // Self-listen worker. nullptr / non-joinable when disabled.
    std::thread                                  self_listen_thread_;
    std::shared_ptr<std::atomic<bool>>           self_listen_stop_;
    std::shared_ptr<std::condition_variable>     self_listen_cv_;

    bool stopped_ = false;
};

// Build the TTS + playback + self-listen stack. Always returns a
// non-null pointer; .enabled() is false when cfg.tts.voices is empty
// or when no playback engine could be constructed.
//
// `playback_active_turn` is shared with the dialogue stack — Manager
// updates it synchronously from its turn-started hook, the playback
// engine reads it on every audio-cb dequeue. Owned by main.cpp so
// both stacks see the same atomic.
[[nodiscard]] std::unique_ptr<TtsStack>
build_tts_stack(const config::Config& cfg,
                 event::EventBus& bus,
                 const std::shared_ptr<metrics::Registry>& registry,
                 const std::shared_ptr<std::atomic<event::TurnId>>& playback_active_turn);

} // namespace acva::orchestrator
