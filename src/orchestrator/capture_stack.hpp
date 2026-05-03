#pragma once

#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/loopback.hpp"
#include "audio/pipeline.hpp"
#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "event/bus.hpp"
#include "metrics/registry.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace acva::orchestrator {

// M4 + M5 + M6 capture path: PortAudio input → SPSC ring →
// AudioPipeline (resample → APM → VAD → Endpointer) → bus events.
// The half-duplex gate (cfg.audio.half_duplex_while_speaking) is
// also installed here because its lifetime is naturally tied to the
// capture engine.
//
// build_capture_stack() always returns a non-null pointer; .enabled()
// is false when cfg.audio.capture_enabled is false (the synthetic
// fake-driver path runs unchanged in that case).
class CaptureStack {
public:
    CaptureStack() = default;
    ~CaptureStack();

    CaptureStack(const CaptureStack&)            = delete;
    CaptureStack& operator=(const CaptureStack&) = delete;
    CaptureStack(CaptureStack&&)                 = delete;
    CaptureStack& operator=(CaptureStack&&)      = delete;

    // Stop in the right order:
    //   1. capture_engine.stop() — no more frames into the ring
    //   2. audio_pipeline.stop() — drain the worker
    //   3. metrics poller join
    // Idempotent.
    void stop();

    // Non-owning accessors. nullptr / false when capture is disabled.
    [[nodiscard]] audio::AudioPipeline* pipeline() noexcept { return pipeline_.get(); }
    [[nodiscard]] bool                  enabled() const noexcept { return pipeline_ != nullptr; }

private:
    friend std::unique_ptr<CaptureStack> build_capture_stack(
        const config::Config&,
        event::EventBus&,
        const std::shared_ptr<metrics::Registry>&,
        dialogue::Fsm&,
        audio::LoopbackSink*);

    std::unique_ptr<audio::MonotonicAudioClock> clock_;
    std::unique_ptr<audio::CaptureRing>         ring_;
    std::unique_ptr<audio::CaptureEngine>       capture_;
    std::unique_ptr<audio::AudioPipeline>       pipeline_;
    std::unique_ptr<audio::HalfDuplexGate>      gate_;

    std::thread       metrics_thread_;
    std::atomic<bool> metrics_stop_{false};

    bool stopped_ = false;
};

// Build the capture path. Always returns a non-null pointer;
// .enabled() is false when cfg.audio.capture_enabled is false.
//
// `loopback` is the AEC reference tap from the TTS stack — pass null
// to disable the APM stage entirely (capture-only mode with no
// playback).
//
// `fsm` is taken by reference because the half-duplex gate
// installs a state observer on it. The CaptureStack does not own
// the FSM; it MUST outlive the CaptureStack.
[[nodiscard]] std::unique_ptr<CaptureStack>
build_capture_stack(const config::Config& cfg,
                     event::EventBus& bus,
                     const std::shared_ptr<metrics::Registry>& registry,
                     dialogue::Fsm& fsm,
                     audio::LoopbackSink* loopback);

} // namespace acva::orchestrator
