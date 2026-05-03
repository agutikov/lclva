#pragma once

#include "audio/pipeline.hpp"
#include "config/config.hpp"
#include "event/bus.hpp"
#include "stt/openai_stt_client.hpp"
#include "stt/realtime_stt_client.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace acva::orchestrator {

// M4B + M5 STT path. Two backends, exactly one of which is active:
//
//   • Streaming (cfg.stt.streaming = true): RealtimeSttClient owns a
//     long-lived WebRTC session against /v1/realtime. The capture
//     pipeline pushes 16 kHz mono chunks into the live sink between
//     SpeechStarted and SpeechEnded. Subscribers on those events
//     drive begin_utterance / end_utterance; partial+final transcripts
//     publish back onto the bus.
//
//   • Request/response (cfg.stt.streaming = false): OpenAiSttClient
//     POSTs each UtteranceReady's audio to /v1/audio/transcriptions
//     on a single I/O worker thread.
//
// Both publish FinalTranscript on the same bus; the dialogue Manager
// consumes it without caring which path produced it.
//
// build_stt_stack() always returns a non-null pointer; .enabled() is
// false when cfg.stt.base_url is empty (synthetic-only paths still
// work via the fake driver).
class SttStack {
public:
    SttStack() = default;
    ~SttStack();

    SttStack(const SttStack&)            = delete;
    SttStack& operator=(const SttStack&) = delete;
    SttStack(SttStack&&)                 = delete;
    SttStack& operator=(SttStack&&)      = delete;

    // Stop in the right order:
    //   1. realtime_stt.stop() — close the WebRTC session
    //   2. request/response worker join (after notifying stop + cv)
    // Idempotent.
    void stop();

    [[nodiscard]] bool enabled() const noexcept {
        return realtime_ != nullptr || request_response_ != nullptr;
    }

private:
    friend std::unique_ptr<SttStack> build_stt_stack(
        const config::Config&,
        event::EventBus&,
        audio::AudioPipeline*,
        std::vector<event::SubscriptionHandle>&);

    std::unique_ptr<stt::RealtimeSttClient> realtime_;
    std::unique_ptr<stt::OpenAiSttClient>   request_response_;

    // Workers + sync primitives for the request/response path.
    std::thread                  worker_;
    std::atomic<bool>            stop_{false};
    std::mutex                   queue_mu_;
    std::condition_variable      queue_cv_;
    std::deque<stt::SttRequest>  queue_;

    bool stopped_ = false;
};

// Build the STT stack. Subscriptions (SpeechStarted/Ended for
// realtime, UtteranceReady for request/response) are appended to
// `subscription_keepalive` so the bus retains them.
//
// `audio_pipeline` is non-null only when capture is enabled; the
// streaming path uses it to install its live audio sink. Pass null
// to skip wiring the live sink (e.g., capture disabled — the
// streaming path then logs a warning and disables itself).
[[nodiscard]] std::unique_ptr<SttStack>
build_stt_stack(const config::Config& cfg,
                 event::EventBus& bus,
                 audio::AudioPipeline* audio_pipeline,
                 std::vector<event::SubscriptionHandle>& subscription_keepalive);

} // namespace acva::orchestrator
