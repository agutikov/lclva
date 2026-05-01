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

    // Always-on append so the rolling pre-buffer stays warm.
    utterance_buffer_.append(resampled);

    if (vad_) {
        last_vad_p_ = vad_->push_frame(resampled);
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
