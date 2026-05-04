#include "audio/pipeline.hpp"

#include "event/event.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cmath>
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

    // M6 — AEC stage. APM requires exactly one 10-ms frame
    // (output_sample_rate/100 samples) per ProcessStream call. soxr
    // at the 48→16 kHz ratio produces variable chunk sizes (e.g.
    // 0/0/0/192/192/106/192/192 in steady state) that never equal
    // 160 exactly, so we accumulate into apm_carry_ and pull complete
    // blocks out. Pre-fix this path used a `resampled.size() == 160`
    // gate that rejected every frame; M6 was a silent no-op in
    // production until 2026-05-03.
    if (apm_ && apm_->aec_active()) {
        const std::size_t apm_frame_samples =
            cfg_.output_sample_rate / 100U;
        apm_carry_.insert(apm_carry_.end(),
                           resampled.begin(), resampled.end());
        std::vector<std::int16_t> cleaned;
        cleaned.reserve(apm_carry_.size());
        std::size_t off = 0;
        while (apm_carry_.size() - off >= apm_frame_samples) {
            std::span<const std::int16_t> chunk{
                apm_carry_.data() + off, apm_frame_samples};
            auto out = apm_->process(chunk, frame.captured_at);
            cleaned.insert(cleaned.end(), out.begin(), out.end());
            off += apm_frame_samples;
        }
        if (off > 0) {
            apm_carry_.erase(
                apm_carry_.begin(),
                apm_carry_.begin() + static_cast<std::ptrdiff_t>(off));
        }
        // Skip downstream if we couldn't form a single 10-ms block —
        // the leftover sits in apm_carry_ for the next process_frame
        // call. At soxr's typical 192-sample output that happens at
        // most once per startup; in steady state every input frame
        // produces ≥ 1 cleaned chunk.
        if (cleaned.empty()) return;
        resampled = std::move(cleaned);
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
            // Snapshot the rolling pre-buffer BEFORE on_speech_started
            // adopts (and clears) it — that's the audio the user
            // produced before VAD's `min_speech_ms` matured. M7 Bug 1:
            // the M5 streaming sink used to miss this window because
            // it only fired between SpeechStarted and SpeechEnded;
            // realtime STT lost ~200-500 ms of leading audio per
            // utterance and occasionally clipped the first phoneme.
            const auto pre_pad = utterance_buffer_.pre_buffer_snapshot();
            utterance_buffer_.on_speech_started(pre_pad_start, frame.captured_at);
            in_speech_ = true;
            // Replay the pre-padding window into the live sink first
            // so the realtime STT sees the same prefix the M4B path
            // keeps in the UtteranceBuffer, then the current frame.
            if (live_sink_) {
                if (!pre_pad.empty()) {
                    live_sink_(pre_pad);
                }
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
                // M7 Bug 4 — RMS gate. Whisper hallucinates subtitle
                // text on near-silent buffers; the M4 endpointer's
                // min_speech_ms catches most of these but not all
                // (pre/post padding can dilute energy below the
                // hallucination floor). Compute RMS, drop the slice
                // when too quiet.
                bool drop_low_rms = false;
                if (cfg_.min_utterance_rms > 0) {
                    const auto samples = slice->samples();
                    if (!samples.empty()) {
                        std::uint64_t sum_sq = 0;
                        for (auto s : samples) {
                            const std::int64_t v = s;
                            sum_sq += static_cast<std::uint64_t>(v * v);
                        }
                        const double rms = std::sqrt(
                            static_cast<double>(sum_sq)
                            / static_cast<double>(samples.size()));
                        if (rms < static_cast<double>(cfg_.min_utterance_rms)) {
                            drop_low_rms = true;
                            low_rms_drops_total_.fetch_add(
                                1, std::memory_order_relaxed);
                            // info-level (not warn) because this is the
                            // expected behaviour for ambient noise
                            // utterances; warn would cry wolf.
                            log::info("audio_pipeline", fmt::format(
                                "dropping low-RMS utterance: rms={:.1f} "
                                "min_rms={} duration_ms={}",
                                rms, cfg_.min_utterance_rms,
                                slice->duration().count()));
                        }
                    }
                }
                if (!drop_low_rms) {
                    utterances_total_.fetch_add(1, std::memory_order_relaxed);
                    bus_.publish(event::UtteranceReady{
                        .turn  = event::kNoTurn,
                        .slice = std::move(slice),
                    });
                }
            }
            break;
        }
    }
}

} // namespace acva::audio
