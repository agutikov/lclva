#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/pipeline.hpp"
#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using acva::audio::AudioPipeline;
using acva::audio::CaptureEngine;
using acva::audio::CaptureRing;
using acva::audio::MonotonicAudioClock;
using namespace std::chrono_literals;

namespace {

acva::config::AudioConfig audio_cfg() {
    acva::config::AudioConfig c;
    c.sample_rate_hz   = 48000;
    c.buffer_frames    = 480;
    c.input_device     = "none";  // headless capture for tests
    c.capture_enabled  = true;
    return c;
}

AudioPipeline::Config pipeline_cfg() {
    AudioPipeline::Config c;
    c.input_sample_rate           = 48000;
    c.output_sample_rate          = 16000;
    c.endpointer.min_speech_ms    = 60ms;     // tighten for fast tests
    c.endpointer.hangover_ms      = 100ms;
    c.endpointer.pre_padding_ms   = 50ms;
    c.endpointer.post_padding_ms  = 30ms;
    c.pre_padding_ms              = 50ms;
    c.post_padding_ms             = 30ms;
    c.max_in_flight               = 3;
    c.max_duration_ms             = 5000ms;
    c.vad_model_path              = "";       // bypass Silero
    return c;
}

// Inject N frames of `sample_count` int16 samples each through the
// CaptureEngine, then synchronously drain via pump_for_test.
std::size_t inject_and_pump(CaptureEngine& cap,
                              AudioPipeline& pipe,
                              std::size_t frames,
                              std::size_t sample_count = 480) {
    std::vector<std::int16_t> buf(sample_count, 1000);
    for (std::size_t i = 0; i < frames; ++i) {
        cap.inject_for_test(buf);
    }
    return pipe.pump_for_test(frames);
}

} // namespace

TEST_CASE("AudioPipeline: drains ring + counts processed frames") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg(), ring, clock, bus);
    const auto processed = inject_and_pump(cap, pipe, 5);
    CHECK(processed == 5);
    CHECK(pipe.frames_processed() == 5);
    CHECK(pipe.utterances_total() == 0);   // VAD off → no speech
}

TEST_CASE("AudioPipeline: forced VAD probability drives SpeechStarted + UtteranceReady") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg(), ring, clock, bus);

    std::atomic<int> speech_started{0};
    std::atomic<int> speech_ended{0};
    std::atomic<int> utterance_ready{0};
    std::shared_ptr<acva::audio::AudioSlice> received;
    auto sub_started = bus.subscribe<acva::event::SpeechStarted>({},
        [&](const acva::event::SpeechStarted&) { speech_started.fetch_add(1); });
    auto sub_ended = bus.subscribe<acva::event::SpeechEnded>({},
        [&](const acva::event::SpeechEnded&) { speech_ended.fetch_add(1); });
    auto sub_ready = bus.subscribe<acva::event::UtteranceReady>({},
        [&](const acva::event::UtteranceReady& e) {
            received = e.slice;
            utterance_ready.fetch_add(1);
        });

    // Drive with high probability for ~120 ms of audio (well over
    // min_speech_ms=60ms) — endpointer should fire SpeechStarted.
    pipe.set_test_probability(0.9F);
    inject_and_pump(cap, pipe, 12);  // 12 × 10 ms = 120 ms

    // Then drop probability for ~150 ms (over hangover_ms=100ms) —
    // should fire SpeechEnded + UtteranceReady.
    pipe.set_test_probability(0.05F);
    inject_and_pump(cap, pipe, 15);

    // Bus subscriptions are async — wait for the dispatcher.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
            && (speech_started.load() == 0 || utterance_ready.load() == 0)) {
        std::this_thread::sleep_for(10ms);
    }

    CHECK(speech_started.load() == 1);
    CHECK(speech_ended.load() == 1);
    CHECK(utterance_ready.load() == 1);
    REQUIRE(received);
    CHECK(received->sample_rate() == 16000);
    CHECK(received->samples().size() > 0);
    CHECK(pipe.utterances_total() == 1);

    bus.shutdown();
}

TEST_CASE("AudioPipeline: false start increments counter, no UtteranceReady") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    // Tighten min_speech_ms so 30 ms doesn't accidentally cross the
    // mature threshold; soxr's startup transient may swallow ~one
    // frame so we use a comfortable margin both ways.
    auto cfg = pipeline_cfg();
    cfg.endpointer.min_speech_ms = 100ms;
    AudioPipeline pipe(cfg, ring, clock, bus);

    std::atomic<int> utterance_ready{0};
    auto sub = bus.subscribe<acva::event::UtteranceReady>({},
        [&](const acva::event::UtteranceReady&) { utterance_ready.fetch_add(1); });

    // ~50 ms above onset — well under min_speech_ms=100ms.
    pipe.set_test_probability(0.8F);
    inject_and_pump(cap, pipe, 5);
    // Then drop below offset — false start.
    pipe.set_test_probability(0.05F);
    inject_and_pump(cap, pipe, 5);

    CHECK(pipe.false_starts_total() == 1);
    CHECK(pipe.utterances_total() == 0);

    std::this_thread::sleep_for(50ms);
    CHECK(utterance_ready.load() == 0);
    bus.shutdown();
}

TEST_CASE("AudioPipeline: ring overrun is observable via CaptureEngine counter") {
    auto a_cfg = audio_cfg();
    acva::event::EventBus bus;
    MonotonicAudioClock clock;
    CaptureRing ring;
    CaptureEngine cap(a_cfg, ring, clock);
    cap.force_headless();
    cap.start();

    AudioPipeline pipe(pipeline_cfg(), ring, clock, bus);

    // Push more frames than the ring's usable capacity (255) without
    // pumping. Excess frames must increment ring_overruns.
    std::vector<std::int16_t> buf(480, 0);
    for (std::size_t i = 0; i < 300; ++i) {
        cap.inject_for_test(buf);
    }
    CHECK(cap.ring_overruns() > 0);

    bus.shutdown();
}
