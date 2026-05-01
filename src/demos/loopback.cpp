#include "demos/demo.hpp"

#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/resampler.hpp"
#include "event/bus.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

namespace acva::demos {

int run_loopback(const config::Config& cfg) {
    using namespace std::chrono_literals;

    constexpr auto kDuration = std::chrono::seconds(5);

    std::printf(
        "demo[loopback] capture device='%s' input rate=%dHz duration=%llds\n",
        cfg.audio.input_device.c_str(),
        static_cast<int>(cfg.audio.sample_rate_hz),
        static_cast<long long>(kDuration.count()));

    if (!cfg.audio.capture_enabled) {
        std::fprintf(stderr,
            "demo[loopback] note: cfg.audio.capture_enabled is false; "
            "running anyway since the demo opens its own input stream.\n");
    }

    event::EventBus bus;
    audio::MonotonicAudioClock clock;
    audio::CaptureRing ring;
    audio::CaptureEngine capture(cfg.audio, ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[loopback] capture.start() failed\n");
        return EXIT_FAILURE;
    }
    std::printf("demo[loopback] capture running (headless=%s)\n",
                 capture.headless() ? "true" : "false");

    playback::PlaybackQueue queue(cfg.playback.max_queue_chunks);
    constexpr event::TurnId kFakeTurn = 1;
    playback::PlaybackEngine engine(cfg.audio, queue, bus,
                                      []{ return kFakeTurn; });
    if (!engine.start()) {
        std::fprintf(stderr, "demo[loopback] engine.start() failed\n");
        capture.stop();
        return EXIT_FAILURE;
    }
    std::printf("demo[loopback] playback running (headless=%s)\n",
                 engine.headless() ? "true" : "false");
    std::printf("demo[loopback] speak now…\n");

    audio::Resampler down(static_cast<double>(cfg.audio.sample_rate_hz), 16000.0);
    audio::Resampler up  (16000.0, static_cast<double>(cfg.audio.sample_rate_hz));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> frames_processed{0};
    event::SequenceNo seq = 0;

    std::thread worker([&]{
        while (!stop.load(std::memory_order_acquire)) {
            auto frame_opt = ring.pop();
            if (!frame_opt) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const auto& frame = *frame_opt;
            frames_processed.fetch_add(frame.count, std::memory_order_relaxed);

            // 48 → 16 → 48: round-trips through soxr to verify the
            // M4 capture path is wired correctly. Without VAD this is
            // pure passthrough; the operator listens for silence
            // dropouts, not endpoint behaviour.
            auto down_samples = down.process(frame.view());
            if (down_samples.empty()) continue;
            auto up_samples = up.process(down_samples);
            if (up_samples.empty()) continue;

            playback::AudioChunk chunk;
            chunk.turn = kFakeTurn;
            chunk.seq  = seq++;
            chunk.samples = std::move(up_samples);
            (void)queue.enqueue(std::move(chunk));
        }
    });

    std::this_thread::sleep_for(kDuration);
    stop.store(true, std::memory_order_release);
    worker.join();

    capture.stop();
    // Brief settle window so the trailing samples render.
    std::this_thread::sleep_for(120ms);
    engine.stop();
    bus.shutdown();

    std::printf(
        "demo[loopback] done: frames_captured=%llu underruns=%llu overruns=%llu\n",
        static_cast<unsigned long long>(frames_processed.load()),
        static_cast<unsigned long long>(engine.underruns()),
        static_cast<unsigned long long>(capture.ring_overruns()));

    if (capture.headless() || engine.headless()) {
        std::printf(
            "demo[loopback] NOTE: capture or playback ran in headless mode.\n");
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
