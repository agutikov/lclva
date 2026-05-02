#include "demos/demo.hpp"

#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/pipeline.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "stt/realtime_stt_client.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace acva::demos {

namespace {

std::atomic<int> g_signal_received{0};
void handle_signal(int sig) noexcept { g_signal_received.store(sig); }

} // namespace

// `acva demo transcribe` — mic → VAD endpointer → realtime STT, prints
// every PartialTranscript (running text) and FinalTranscript (final
// text + timing) as they arrive. Same exact wiring as the production
// `acva` binary's STT path, just isolated from LLM/TTS so STT
// behavior can be diagnosed without the dialogue pipeline interfering.
//
// Runs for 30 s by default; Ctrl-C exits early.
int run_transcribe(const config::Config& cfg) {
    using namespace std::chrono_literals;
    constexpr auto kDuration = std::chrono::seconds(30);

    if (cfg.stt.base_url.empty()) {
        std::fprintf(stderr,
            "demo[transcribe] FAIL: cfg.stt.base_url is empty — Speaches not configured\n");
        return EXIT_FAILURE;
    }
    if (cfg.vad.model_path.empty()) {
        std::fprintf(stderr,
            "demo[transcribe] WARNING: cfg.vad.model_path is empty — VAD will not "
            "fire on real speech. Run scripts/download-vad.sh.\n");
    }

    std::printf(
        "demo[transcribe] device='%s' model=%s duration=%llds\n",
        cfg.audio.input_device.c_str(),
        cfg.stt.model.c_str(),
        static_cast<long long>(kDuration.count()));

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    event::EventBus bus;
    audio::MonotonicAudioClock clock;
    audio::CaptureRing ring;
    audio::CaptureEngine capture(cfg.audio, ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[transcribe] capture.start() failed\n");
        return EXIT_FAILURE;
    }
    if (capture.headless()) {
        std::fprintf(stderr,
            "demo[transcribe] capture is headless — no audio reaches the pipeline. "
            "Check audio.input_device.\n");
        capture.stop();
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

    stt::RealtimeSttClient stt_client(cfg.stt);
    if (!stt_client.start(std::chrono::seconds(15))) {
        std::fprintf(stderr,
            "demo[transcribe] FAIL: realtime stt did not reach Ready — is Speaches up?\n");
        capture.stop();
        return EXIT_FAILURE;
    }

    // Wire the audio pipeline's live sink directly to the STT client's
    // push_audio. Same flow as main.cpp.
    pipeline.set_live_audio_sink(
        [&stt_client](std::span<const std::int16_t> samples) {
            stt_client.push_audio(samples);
        });

    pipeline.start();

    // Counters for the summary at the end.
    std::atomic<std::size_t> partials_seen{0};
    std::atomic<std::size_t> finals_seen{0};
    std::mutex print_mu; // serialize stdout writes from bus subscribers

    auto sub_started = bus.subscribe<event::SpeechStarted>({},
        [&](const event::SpeechStarted&) {
            auto cancel = std::make_shared<dialogue::CancellationToken>();
            stt::RealtimeSttClient::UtteranceCallbacks cb;
            cb.on_partial = [&](event::PartialTranscript p) {
                std::lock_guard lk(print_mu);
                std::printf("  partial  seq=%u  %s\n",
                             p.seq, p.text.c_str());
                std::fflush(stdout);
                partials_seen.fetch_add(1);
            };
            cb.on_final = [&](event::FinalTranscript f) {
                std::lock_guard lk(print_mu);
                std::printf("  final    [%6lld ms / %6lld ms]  lang=%-3s  %s\n",
                             static_cast<long long>(f.audio_duration.count()),
                             static_cast<long long>(f.processing_duration.count()),
                             f.lang.empty() ? "?" : f.lang.c_str(),
                             f.text.c_str());
                std::fflush(stdout);
                finals_seen.fetch_add(1);
            };
            cb.on_error = [&](std::string err) {
                std::lock_guard lk(print_mu);
                std::printf("  ERROR    %s\n", err.c_str());
                std::fflush(stdout);
            };
            stt_client.begin_utterance(event::kNoTurn, cancel, std::move(cb));
            std::lock_guard lk(print_mu);
            std::printf("[speech started]\n");
            std::fflush(stdout);
        });
    auto sub_ended = bus.subscribe<event::SpeechEnded>({},
        [&stt_client, &print_mu](const event::SpeechEnded&) {
            stt_client.end_utterance();
            std::lock_guard lk(print_mu);
            std::printf("[speech ended — waiting for transcript…]\n");
            std::fflush(stdout);
        });

    {
        std::lock_guard lk(print_mu);
        std::printf("demo[transcribe] speak when ready (Ctrl-C to exit)…\n");
        std::fflush(stdout);
    }

    const auto deadline = std::chrono::steady_clock::now() + kDuration;
    while (std::chrono::steady_clock::now() < deadline
            && g_signal_received.load() == 0) {
        std::this_thread::sleep_for(100ms);
    }

    pipeline.stop();
    capture.stop();
    stt_client.stop();
    bus.shutdown();

    std::printf("\ndemo[transcribe] done: partials=%zu finals=%zu utterances=%llu\n",
                 partials_seen.load(),
                 finals_seen.load(),
                 static_cast<unsigned long long>(pipeline.utterances_total()));
    return EXIT_SUCCESS;
}

} // namespace acva::demos
