#include "demos/demo.hpp"

#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <utility>

namespace acva::demos {

int run_tone(const config::Config& cfg, std::span<const std::string> /*args*/) {
    using namespace std::chrono_literals;

    const int          rate          = static_cast<int>(cfg.audio.sample_rate_hz);
    const std::size_t  buf           = cfg.audio.buffer_frames;
    constexpr double   kFreqHz       = 440.0;
    constexpr double   kDurationSec  = 1.5;
    constexpr double   kAmplitude    = 0.30;        // -10 dBFS — comfortable headroom

    const std::size_t total = static_cast<std::size_t>(
        static_cast<double>(rate) * kDurationSec);

    std::printf(
        "demo[tone] %d Hz at %d Hz sample rate, %.1f s, device='%s'\n",
        static_cast<int>(kFreqHz), rate, kDurationSec,
        cfg.audio.output_device.c_str());

    event::EventBus bus;
    playback::PlaybackQueue queue(cfg.playback.max_queue_chunks);
    constexpr event::TurnId kFakeTurn = 1;
    playback::PlaybackEngine engine(cfg.audio, cfg.playback, queue, bus,
                                      []{ return kFakeTurn; });
    if (!engine.start()) {
        std::fprintf(stderr, "demo[tone] engine.start() failed\n");
        return EXIT_FAILURE;
    }
    std::printf("demo[tone] engine running (headless=%s)\n",
                 engine.headless() ? "true" : "false");

    // Stream the sine in `buf`-sized chunks so the engine sees the
    // same chunk granularity as a real TTS run.
    event::SequenceNo seq = 0;
    for (std::size_t i = 0; i < total; ) {
        const std::size_t n = std::min(buf, total - i);
        playback::AudioChunk chunk;
        chunk.turn = kFakeTurn;
        chunk.seq  = seq;
        chunk.samples.resize(n);
        for (std::size_t j = 0; j < n; ++j) {
            const double t = static_cast<double>(i + j)
                               / static_cast<double>(rate);
            const double s = std::sin(
                2.0 * std::numbers::pi_v<double> * kFreqHz * t);
            chunk.samples[j] = static_cast<std::int16_t>(
                std::lround(s * kAmplitude * 32760.0));
        }
        // Backpressure-safe enqueue: PlaybackQueue::enqueue moves the
        // chunk in even on a failed push (rejected by capacity), so we
        // can't naively retry. Pre-check `size() < capacity` instead —
        // safe because this demo is the only producer; the engine
        // draining only ever lowers `size`.
        while (queue.size() >= queue.capacity()) {
            std::this_thread::sleep_for(10ms);
        }
        (void)queue.enqueue(std::move(chunk));
        ++seq;
        i += n;
    }

    // Wait for playback to drain. The engine's chunks_played counter
    // increments per fully-consumed chunk; we expect it to reach `seq`
    // (or close to it — the very last chunk may be reported only after
    // the next dequeue attempt).
    const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::duration<double>(kDurationSec + 1.0));
    while (engine.chunks_played() + 1 < static_cast<std::uint64_t>(seq)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    // Brief settle window so the trailing samples get rendered.
    std::this_thread::sleep_for(120ms);
    engine.stop();
    bus.shutdown();

    std::printf(
        "demo[tone] done: chunks_enqueued=%u chunks_played=%llu "
        "underruns=%llu drops=%llu\n",
        static_cast<unsigned>(seq),
        static_cast<unsigned long long>(engine.chunks_played()),
        static_cast<unsigned long long>(engine.underruns()),
        static_cast<unsigned long long>(queue.drops()));

    if (engine.headless()) {
        std::printf(
            "demo[tone] NOTE: engine ran in headless mode — no audio reached "
            "the speakers. Set audio.output_device to a real device to hear it.\n");
        return EXIT_SUCCESS;     // not a failure; the user asked for it
    }
    if (engine.chunks_played() == 0) {
        std::fprintf(stderr,
            "demo[tone] FAIL: engine consumed zero chunks "
            "(check that the device matches the configured rate)\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
