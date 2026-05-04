#include "demos/demo.hpp"

#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/pipeline.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

struct UtteranceLog {
    std::chrono::steady_clock::time_point started_at{};
    std::chrono::steady_clock::time_point ended_at{};
    std::size_t                           samples = 0;
    std::uint32_t                         sample_rate = 16000;
};

} // namespace

int run_capture(const config::Config& cfg) {
    using namespace std::chrono_literals;

    constexpr auto kDuration = std::chrono::seconds(5);

    std::printf(
        "demo[capture] device='%s' duration=%llds vad onset>=%.2f offset<=%.2f hangover=%ums\n",
        cfg.audio.input_device.c_str(),
        static_cast<long long>(kDuration.count()),
        static_cast<double>(cfg.vad.onset_threshold),
        static_cast<double>(cfg.vad.offset_threshold),
        cfg.vad.hangover_ms);

    if (cfg.vad.model_path.empty()) {
        std::fprintf(stderr,
            "demo[capture] WARNING: cfg.vad.model_path is empty — VAD will not "
            "fire on real speech. Run `tools/acva-models install silero-v5` "
            "and set cfg.vad.model_path.\n");
    }

    event::EventBus bus;
    audio::MonotonicAudioClock clock;
    audio::CaptureRing ring;
    audio::CaptureEngine capture(cfg.audio, ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[capture] capture.start() failed\n");
        return EXIT_FAILURE;
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

    audio::AudioPipeline pipeline(std::move(apc), ring, clock, bus);
    pipeline.start();

    std::printf("demo[capture] speak when ready…\n");

    std::mutex log_mu;
    std::vector<UtteranceLog> log;
    UtteranceLog pending{};

    auto started_sub = bus.subscribe<event::SpeechStarted>({},
        [&](const event::SpeechStarted& e) {
            std::lock_guard lk(log_mu);
            pending = {};
            pending.started_at = e.ts;
        });
    auto ready_sub = bus.subscribe<event::UtteranceReady>({},
        [&](const event::UtteranceReady& e) {
            std::lock_guard lk(log_mu);
            if (!e.slice) return;
            pending.ended_at    = e.slice->ended_at();
            pending.samples     = e.slice->samples().size();
            pending.sample_rate = e.slice->sample_rate();
            log.push_back(pending);
        });

    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(kDuration);

    capture.stop();
    pipeline.stop();
    bus.shutdown();

    std::printf("demo[capture] done: utterances=%zu false_starts=%llu drops=%llu\n",
                 log.size(),
                 static_cast<unsigned long long>(pipeline.false_starts_total()),
                 static_cast<unsigned long long>(pipeline.utterance_drops()));

    int idx = 0;
    for (const auto& u : log) {
        const double t_start = std::chrono::duration<double>(u.started_at - t0).count();
        const double t_end   = std::chrono::duration<double>(u.ended_at   - t0).count();
        const double dur     = u.sample_rate > 0
                                 ? static_cast<double>(u.samples)
                                       / static_cast<double>(u.sample_rate)
                                 : 0.0;
        std::printf("  utterance #%d: %.3fs → %.3fs  (%.2fs, %zu samples @ %u Hz)\n",
                     ++idx, t_start, t_end, dur, u.samples, u.sample_rate);
    }

    if (capture.headless()) {
        std::printf(
            "demo[capture] NOTE: capture ran in headless mode — no audio reached "
            "the pipeline. Set audio.input_device to a real device to capture.\n");
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
