#pragma once

#include "event/bus.hpp"
#include "event/event.hpp"

#include <prometheus/counter.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace acva::metrics {

// Wraps prometheus::Registry and the metric families we use across the
// orchestrator. Constructed once at startup and shared (by reference) by all
// components that emit metrics.
//
// Family names follow the project convention `voice_*` from §12 of the
// design. Labels are chosen for low cardinality (e.g., `event` is one of a
// fixed set of event names).
class Registry {
public:
    Registry();

    // The underlying prometheus registry, exposed for HTTP serialization
    // and ad-hoc registration of additional Collectables.
    [[nodiscard]] std::shared_ptr<prometheus::Registry> registry() const noexcept {
        return registry_;
    }

    // Increment the published-events counter for `event_name`.
    void on_event_published(const char* event_name);

    // Mark a turn outcome. `outcome` is one of: completed, interrupted, discarded.
    void on_turn_outcome(const char* outcome);

    // Drops on a queue (event bus subscription queue, etc.).
    void on_queue_drop(const std::string& queue_name);

    // Service restart count (called by supervisor in M2).
    void on_service_restart(const std::string& service_name);

    // Update an FSM state gauge (1 for current state, 0 elsewhere). Called
    // by the FSM observer.
    void set_fsm_state(const char* state_name);

    // M2 — supervisor health metrics. ----------------------------------

    // Pre-create a service's health-state gauge so /metrics shows all
    // four label values from t=0 onwards. Called once per registered
    // service at startup. Without this, prometheus would only show a
    // service after its state transitions.
    void register_service_for_health(const std::string& service);

    // Set the health gauge for `service` to `state`. Mirrors set_fsm_state.
    void set_health_state(const std::string& service, const char* state);

    // Increment the keep-alive ping counter (fired vs skipped).
    void on_keep_alive(bool fired);

    // Set the pipeline-state gauge (one of: ok, failed,
    // no_configured_critical_services).
    void set_pipeline_state(const char* state);

    // M3 — TTS / playback metrics. -------------------------------------

    // Observed latency from TtsStarted to the first TtsAudioChunk for
    // a (turn, seq) pair, in milliseconds. Subscribed automatically via
    // subscribe(bus); main.cpp doesn't need to call this directly.
    void on_tts_first_audio(double ms);

    // Cumulative byte count of TTS audio that reached the playback
    // queue. Driven from TtsAudioChunk events.
    void on_tts_audio_bytes(std::uint64_t delta_bytes);

    // Polled gauges populated by a poller thread in main.cpp from the
    // PlaybackEngine + PlaybackQueue counters. Set absolute values.
    void set_playback_queue_depth(double depth);
    void set_playback_underruns_total(double total);
    void set_playback_chunks_played_total(double total);
    void set_playback_drops_total(double total);

    // M4 — capture / VAD / utterance metrics. All polled from the
    // AudioPipeline + CaptureEngine + UtteranceBuffer counters by the
    // same main.cpp poller thread that handles playback. Set absolute
    // values; the registry mirrors them onto monotonic gauges so a
    // process restart resets the dashboard to 0.
    void set_capture_frames_total(double total);
    void set_capture_ring_overruns_total(double total);
    void set_audio_pipeline_frames_total(double total);
    void set_vad_false_starts_total(double total);
    void set_utterances_total(double total);
    void set_utterance_drops_total(double total);
    void set_utterance_in_flight(double depth);

    // M6 — AEC metrics. Polled from Apm::aec_delay_estimate_ms() and
    // erle_db() by the same audio-pipeline polling thread. The delay
    // gauge is the APM's instantaneous internal estimate (refines from
    // cfg.apm.initial_delay_estimate_ms over the first ~3 s of
    // playback). ERLE (echo return loss enhancement) is dB lower than
    // input — higher = more echo cancelled. Acceptance gate is
    // > 25 dB after convergence.
    void set_aec_delay_estimate_ms(double ms);
    void set_aec_erle_db(double db);
    void set_aec_frames_processed_total(double total);

    // M7 — barge-in metrics. ----------------------------------------
    //
    // `voice_barge_in_latency_ms` — UserInterrupted publish → first
    // post-cancel silent audio buffer emitted by PlaybackEngine. The
    // M7 acceptance gate is P50 ≤ 200 ms / P95 ≤ 400 ms (project_design
    // §19). Observed once per fired barge-in.
    void on_barge_in_latency(double ms);

    // Polled gauges populated by main.cpp's metrics thread from the
    // BargeInDetector counters. Set absolute values; restart resets to 0.
    void set_barge_in_fires_total(double total);
    void set_barge_in_suppressed_total(double total);
    void set_barge_in_suppressed_cooldown(double total);
    void set_barge_in_suppressed_aec(double total);

    // Subscribe metrics-collection handlers to the bus. Call after
    // construction. Returns subscriptions which the caller must keep alive.
    [[nodiscard]] std::vector<event::SubscriptionHandle> subscribe(event::EventBus& bus);

private:
    std::shared_ptr<prometheus::Registry> registry_;

    prometheus::Family<prometheus::Counter>*   events_published_;
    prometheus::Family<prometheus::Counter>*   turns_total_;
    prometheus::Family<prometheus::Counter>*   queue_drops_;
    prometheus::Family<prometheus::Counter>*   service_restarts_;
    prometheus::Family<prometheus::Counter>*   keep_alive_total_;
    prometheus::Family<prometheus::Gauge>*     fsm_state_;
    prometheus::Family<prometheus::Gauge>*     health_state_;
    prometheus::Family<prometheus::Gauge>*     pipeline_state_;
    prometheus::Family<prometheus::Histogram>* llm_first_token_ms_;
    prometheus::Family<prometheus::Histogram>* llm_tokens_per_sec_;
    prometheus::Histogram* llm_first_token_ms_metric_ = nullptr;
    prometheus::Histogram* llm_tokens_per_sec_metric_ = nullptr;

    prometheus::Family<prometheus::Histogram>* tts_first_audio_ms_   = nullptr;
    prometheus::Histogram*                     tts_first_audio_ms_metric_ = nullptr;
    prometheus::Family<prometheus::Counter>*   tts_audio_bytes_      = nullptr;
    prometheus::Counter*                       tts_audio_bytes_metric_ = nullptr;
    prometheus::Family<prometheus::Gauge>*     playback_queue_depth_ = nullptr;
    prometheus::Gauge*                         playback_queue_depth_metric_ = nullptr;
    prometheus::Family<prometheus::Gauge>*     playback_underruns_   = nullptr;
    prometheus::Gauge*                         playback_underruns_metric_   = nullptr;
    prometheus::Family<prometheus::Gauge>*     playback_chunks_played_ = nullptr;
    prometheus::Gauge*                         playback_chunks_played_metric_ = nullptr;
    prometheus::Family<prometheus::Gauge>*     playback_drops_       = nullptr;
    prometheus::Gauge*                         playback_drops_metric_       = nullptr;

    // M4 capture / VAD / utterance gauges. All driven by main.cpp's
    // poller thread reading AudioPipeline + CaptureEngine + UtteranceBuffer
    // counters every 500 ms.
    prometheus::Family<prometheus::Gauge>* capture_frames_total_           = nullptr;
    prometheus::Gauge*                     capture_frames_total_metric_    = nullptr;
    prometheus::Family<prometheus::Gauge>* capture_ring_overruns_          = nullptr;
    prometheus::Gauge*                     capture_ring_overruns_metric_   = nullptr;
    prometheus::Family<prometheus::Gauge>* audio_pipeline_frames_          = nullptr;
    prometheus::Gauge*                     audio_pipeline_frames_metric_   = nullptr;
    prometheus::Family<prometheus::Gauge>* vad_false_starts_               = nullptr;
    prometheus::Gauge*                     vad_false_starts_metric_        = nullptr;
    prometheus::Family<prometheus::Gauge>* utterances_total_gauge_         = nullptr;
    prometheus::Gauge*                     utterances_total_metric_        = nullptr;
    prometheus::Family<prometheus::Gauge>* utterance_drops_                = nullptr;
    prometheus::Gauge*                     utterance_drops_metric_         = nullptr;
    prometheus::Family<prometheus::Gauge>* utterance_in_flight_            = nullptr;
    prometheus::Gauge*                     utterance_in_flight_metric_     = nullptr;

    // M6 AEC gauges.
    prometheus::Family<prometheus::Gauge>* aec_delay_estimate_              = nullptr;
    prometheus::Gauge*                     aec_delay_estimate_metric_       = nullptr;
    prometheus::Family<prometheus::Gauge>* aec_erle_db_                     = nullptr;
    prometheus::Gauge*                     aec_erle_db_metric_              = nullptr;
    prometheus::Family<prometheus::Gauge>* aec_frames_processed_            = nullptr;
    prometheus::Gauge*                     aec_frames_processed_metric_     = nullptr;

    // M7 barge-in.
    prometheus::Family<prometheus::Histogram>* barge_in_latency_ms_             = nullptr;
    prometheus::Histogram*                     barge_in_latency_ms_metric_      = nullptr;
    prometheus::Family<prometheus::Gauge>*     barge_in_fires_                  = nullptr;
    prometheus::Gauge*                         barge_in_fires_metric_           = nullptr;
    prometheus::Family<prometheus::Gauge>*     barge_in_suppressed_             = nullptr;
    prometheus::Gauge*                         barge_in_suppressed_metric_      = nullptr;
    prometheus::Gauge*                         barge_in_suppressed_cooldown_metric_ = nullptr;
    prometheus::Gauge*                         barge_in_suppressed_aec_metric_      = nullptr;

    // Per-(turn, seq) TTS timer state, captured between TtsStarted and
    // the first TtsAudioChunk so we can compute first-audio latency.
    // Cleared once the chunk arrives or the turn is interrupted.
    struct TtsTimer {
        std::chrono::steady_clock::time_point started_at{};
        bool first_audio_seen = false;
    };
    std::mutex tts_timers_mu_;
    std::unordered_map<std::uint64_t, TtsTimer> tts_timers_;

    // Services registered via register_service_for_health(). Used so
    // set_health_state can clear all stale labels when a service flips.
    std::mutex health_mu_;
    std::vector<std::string> health_services_;

    // Per-turn timing state captured between LlmStarted / LlmToken /
    // LlmFinished. Single mutex covers both maps; events are infrequent
    // relative to other contention.
    struct TurnTimer {
        std::chrono::steady_clock::time_point started_at{};
        bool first_token_seen = false;
    };
    std::mutex timers_mu_;
    std::unordered_map<event::TurnId, TurnTimer> timers_;
};

} // namespace acva::metrics
