#include "audio/pipeline.hpp"

#include "event/event.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <utility>

namespace acva::audio {

AudioPipeline::AudioPipeline(Config cfg,
                              CaptureRing& ring,
                              MonotonicAudioClock& clock,
                              event::EventBus& bus)
    : cfg_(std::move(cfg)),
      ring_(ring),
      clock_(clock),
      bus_(bus),
      resampler_(static_cast<double>(cfg_.input_sample_rate),
                  static_cast<double>(cfg_.output_sample_rate)),
      endpointer_(cfg_.endpointer, cfg_.output_sample_rate),
      utterance_buffer_(cfg_.output_sample_rate,
                         cfg_.pre_padding_ms,
                         cfg_.post_padding_ms,
                         cfg_.max_in_flight,
                         cfg_.max_duration_ms) {
    if (!cfg_.vad_model_path.empty()) {
        try {
            vad_ = std::make_unique<SileroVad>(cfg_.vad_model_path,
                                                 cfg_.output_sample_rate);
            log::info("audio.pipeline",
                fmt::format("Silero VAD loaded from {}", cfg_.vad_model_path));
        } catch (const std::exception& ex) {
            log::warn("audio.pipeline",
                fmt::format("VAD init failed ({}): VAD disabled, "
                             "endpointing will not fire on real speech",
                             ex.what()));
        }
    }

    // M6 — only construct the APM when there's a loopback to pull
    // reference frames from. The orchestrator always passes one when
    // the playback path is wired; capture-only tests skip it.
    if (cfg_.loopback != nullptr) {
        ApmConfig apm_cfg = cfg_.apm;
        apm_cfg.near_sample_rate_hz =
            static_cast<int>(cfg_.output_sample_rate);
        apm_ = std::make_unique<Apm>(apm_cfg, cfg_.loopback);
    }
}

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    worker_ = std::thread([this] { run_loop(); });
}

void AudioPipeline::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (worker_.joinable()) worker_.join();
}

void AudioPipeline::run_loop() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_acquire)) {
        auto frame_opt = ring_.pop();
        if (!frame_opt) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        process_frame(*frame_opt);
    }
    // Drain the ring on shutdown so we don't lose late-arriving
    // frames (mainly cosmetic for tests).
    while (auto frame_opt = ring_.pop()) {
        process_frame(*frame_opt);
    }
}

std::size_t AudioPipeline::pump_for_test(std::size_t max_frames) {
    std::size_t n = 0;
    while (n < max_frames) {
        auto frame_opt = ring_.pop();
        if (!frame_opt) break;
        process_frame(*frame_opt);
        ++n;
    }
    return n;
}

void AudioPipeline::process_frame(const AudioFrame& frame) {
    frames_processed_.fetch_add(1, std::memory_order_relaxed);

    auto resampled = resampler_.process(frame.view());
    if (resampled.empty()) return;

    // M6 — AEC stage. APM expects exactly one 10 ms frame
    // (output_sample_rate/100 samples) per call. Production input is
    // 480 samples at 48 kHz → 160 samples at 16 kHz, which matches
    // exactly. Soxr can produce a non-160 chunk on the very first few
    // calls (warm-up); we pass those through unchanged. The chunked
    // case is rare enough in practice that the simpler "skip-on-mismatch"
    // policy beats a buffered chunker — at most a few startup frames
    // miss AEC, and they're silence anyway.
    const std::size_t apm_frame_samples =
        cfg_.output_sample_rate / 100U;
    if (apm_ && apm_->aec_active()
        && resampled.size() == apm_frame_samples) {
        resampled = apm_->process(resampled, frame.captured_at);
    }

    // Always-on append so the rolling pre-buffer stays warm.
    utterance_buffer_.append(resampled);

    // M5 streaming-STT sink: invoked while between SpeechStarted and
    // SpeechEnded so the realtime STT client receives audio as it
    // arrives. Coexists with the M4B request/response path on the
    // UtteranceBuffer.
    if (in_speech_ && live_sink_) {
        live_sink_(resampled);
    }

    if (vad_) {
        last_vad_p_ = vad_->push_frame(resampled);
    }
    if (test_probability_ >= 0.0F) {
        last_vad_p_ = test_probability_;
    }

    // Compute frame duration in ms from the resampled length and rate.
    const auto frame_dur = std::chrono::milliseconds{
        static_cast<std::int64_t>(resampled.size()) * 1000
        / static_cast<std::int64_t>(cfg_.output_sample_rate)};

    const auto outcome =
        endpointer_.on_frame(last_vad_p_, frame_dur, frame.captured_at);

    switch (outcome) {
        using FO = Endpointer::FrameOutcome;
        case FO::None:
            break;
        case FO::SpeechStarted: {
            const auto pre_pad_start =
                frame.captured_at - cfg_.pre_padding_ms;
            utterance_buffer_.on_speech_started(pre_pad_start, frame.captured_at);
            in_speech_ = true;
            // Replay the pre-padding window into the live sink so the
            // realtime STT sees the same prefix the M4B path keeps in
            // the UtteranceBuffer. push_audio chunks are short int16
            // mono at the resampler output rate (16 kHz).
            if (live_sink_) {
                live_sink_(resampled);
            }
            bus_.publish(event::SpeechStarted{
                .turn = event::kNoTurn,
                .ts   = frame.captured_at,
            });
            break;
        }
        case FO::FalseStart:
            false_starts_total_.fetch_add(1, std::memory_order_relaxed);
            break;
        case FO::SpeechEnded: {
            auto slice = utterance_buffer_.on_speech_ended(frame.captured_at);
            in_speech_ = false;
            bus_.publish(event::SpeechEnded{
                .turn = event::kNoTurn,
                .ts   = frame.captured_at,
            });
            if (slice) {
                utterances_total_.fetch_add(1, std::memory_order_relaxed);
                bus_.publish(event::UtteranceReady{
                    .turn  = event::kNoTurn,
                    .slice = std::move(slice),
                });
            }
            break;
        }
    }
}

} // namespace acva::audio
