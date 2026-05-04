#include "orchestrator/capture_stack.hpp"

#include "audio/apm.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>

namespace acva::orchestrator {

CaptureStack::~CaptureStack() { stop(); }

void CaptureStack::stop() {
    if (stopped_) return;
    stopped_ = true;

    // capture before pipeline so no more frames land in the SPSC ring
    // while the worker thread is winding down.
    if (capture_)  capture_->stop();
    if (pipeline_) pipeline_->stop();
    if (metrics_thread_.joinable()) {
        metrics_stop_.store(true, std::memory_order_release);
        metrics_thread_.join();
    }
}

std::unique_ptr<CaptureStack>
build_capture_stack(const config::Config& cfg,
                     event::EventBus& bus,
                     const std::shared_ptr<metrics::Registry>& registry,
                     dialogue::Fsm& fsm,
                     audio::LoopbackSink* loopback) {
    auto stack = std::make_unique<CaptureStack>();

    if (!cfg.audio.capture_enabled) {
        log::info("main", "audio capture disabled");
        return stack;     // .enabled() == false
    }

    stack->clock_   = std::make_unique<audio::MonotonicAudioClock>();
    stack->ring_    = std::make_unique<audio::CaptureRing>();
    stack->capture_ = std::make_unique<audio::CaptureEngine>(
        cfg.audio, *stack->ring_, *stack->clock_);

    // M5 half-duplex (speakers without AEC). When enabled, the FSM
    // informs the gate of Speaking transitions and the capture engine
    // drops mic samples while it's active. No-op unless
    // cfg.audio.half_duplex_while_speaking is true.
    if (cfg.audio.half_duplex_while_speaking) {
        stack->gate_ = std::make_unique<audio::HalfDuplexGate>(
            std::chrono::milliseconds{cfg.audio.half_duplex_hangover_ms});
        stack->capture_->set_half_duplex_gate(stack->gate_.get());
        fsm.set_state_observer(
            [g = stack->gate_.get()](dialogue::State prev, dialogue::State next) {
                const bool was_speaking = (prev == dialogue::State::Speaking);
                const bool is_speaking  = (next == dialogue::State::Speaking);
                if (was_speaking != is_speaking) {
                    g->set_speaking(is_speaking);
                }
            });
        log::info("main", fmt::format(
            "half-duplex mode enabled (hangover={}ms)",
            cfg.audio.half_duplex_hangover_ms));
    }

    audio::AudioPipeline::Config apc;
    apc.input_sample_rate           = cfg.audio.sample_rate_hz;
    apc.output_sample_rate          = 16000;
    apc.endpointer.onset_threshold  = cfg.vad.onset_threshold;
    apc.endpointer.offset_threshold = cfg.vad.offset_threshold;
    apc.endpointer.min_speech_ms    = std::chrono::milliseconds{cfg.vad.min_speech_ms};
    apc.endpointer.hangover_ms      = std::chrono::milliseconds{cfg.vad.hangover_ms};
    apc.endpointer.pre_padding_ms   = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.endpointer.post_padding_ms  = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.pre_padding_ms              = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.post_padding_ms             = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.max_in_flight               = cfg.utterance.max_in_flight;
    apc.max_duration_ms             = std::chrono::milliseconds{cfg.utterance.max_duration_ms};
    apc.vad_model_path              = cfg.vad.model_path;
    // M7 Bug 4 — STT-side RMS gate against Whisper hallucinations.
    apc.min_utterance_rms           = cfg.stt.min_utterance_rms;

    // M6 — install the AEC stage when both playback (loopback ring)
    // and an APM-capable build are present. cfg.apm controls the
    // per-subsystem switches; the wrapper is a no-op when the
    // webrtc-audio-processing-1 package wasn't found at build time.
    apc.loopback                       = loopback;
    apc.apm.aec_enabled                = cfg.apm.aec_enabled;
    apc.apm.ns_enabled                 = cfg.apm.ns_enabled;
    apc.apm.agc_enabled                = cfg.apm.agc_enabled;
    apc.apm.initial_delay_estimate_ms  = static_cast<int>(cfg.apm.initial_delay_estimate_ms);
    apc.apm.max_delay_ms               = static_cast<int>(cfg.apm.max_delay_ms);

    stack->pipeline_ = std::make_unique<audio::AudioPipeline>(
        std::move(apc), *stack->ring_, *stack->clock_, bus);

    stack->capture_->start();
    stack->pipeline_->start();

    // Mirror M4 + M6 counters into /metrics every 500 ms. Same shape
    // as the playback poller — main owns the bridge so the capture
    // audio thread never touches prometheus families.
    stack->metrics_thread_ = std::thread([stack_ptr = stack.get(), registry]{
        using namespace std::chrono_literals;
        while (!stack_ptr->metrics_stop_.load(std::memory_order_acquire)) {
            registry->set_capture_frames_total(
                static_cast<double>(stack_ptr->capture_->frames_captured()));
            registry->set_capture_ring_overruns_total(
                static_cast<double>(stack_ptr->capture_->ring_overruns()));
            registry->set_audio_pipeline_frames_total(
                static_cast<double>(stack_ptr->pipeline_->frames_processed()));
            registry->set_vad_false_starts_total(
                static_cast<double>(stack_ptr->pipeline_->false_starts_total()));
            registry->set_utterances_total(
                static_cast<double>(stack_ptr->pipeline_->utterances_total()));
            registry->set_utterance_drops_total(
                static_cast<double>(stack_ptr->pipeline_->utterance_drops()));
            registry->set_utterance_in_flight(
                static_cast<double>(stack_ptr->pipeline_->in_flight()));

            // M6 — surface APM stats whenever the AEC stage is wired
            // in. Runs the gauges off the same poller so we don't need
            // a second thread; the 500 ms cadence is fine because
            // APM's internal estimator updates over seconds.
            if (const auto* apm = stack_ptr->pipeline_->apm(); apm != nullptr) {
                registry->set_aec_delay_estimate_ms(
                    static_cast<double>(apm->aec_delay_estimate_ms()));
                registry->set_aec_erle_db(
                    static_cast<double>(apm->erle_db()));
                registry->set_aec_frames_processed_total(
                    static_cast<double>(apm->frames_processed()));
            }
            std::this_thread::sleep_for(500ms);
        }
    });

    log::info("main", fmt::format(
        "audio capture enabled (input='{}', headless={}, vad={})",
        cfg.audio.input_device,
        stack->capture_->headless(),
        stack->pipeline_->vad_enabled() ? "silero" : "off"));
    return stack;
}

} // namespace acva::orchestrator
