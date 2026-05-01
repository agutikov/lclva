#include "audio/endpointer.hpp"

#include <doctest/doctest.h>

#include <chrono>

using acva::audio::Endpointer;
using acva::audio::EndpointerConfig;
using acva::audio::EndpointerState;
using FrameOutcome = Endpointer::FrameOutcome;
using namespace std::chrono_literals;

namespace {

EndpointerConfig default_cfg() {
    EndpointerConfig c;
    c.onset_threshold  = 0.5F;
    c.offset_threshold = 0.35F;
    c.min_speech_ms    = 200ms;
    c.hangover_ms      = 600ms;
    c.pre_padding_ms   = 300ms;
    c.post_padding_ms  = 100ms;
    return c;
}

// Sequence one frame at a time, feeding `prob` for `duration` (single
// frame). Returns the resulting outcome.
FrameOutcome step(Endpointer& ep, float prob,
                   std::chrono::milliseconds dur,
                   std::chrono::steady_clock::time_point& clock) {
    auto out = ep.on_frame(prob, dur, clock);
    clock += dur;
    return out;
}

} // namespace

TEST_CASE("Endpointer: starts in Quiet and stays there for low-prob frames") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    for (int i = 0; i < 10; ++i) {
        CHECK(step(ep, 0.10F, 32ms, t) == FrameOutcome::None);
    }
    CHECK(ep.state() == EndpointerState::Quiet);
}

TEST_CASE("Endpointer: requires min_speech_ms of speech before SpeechStarted") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};

    CHECK(step(ep, 0.80F, 32ms, t) == FrameOutcome::None);
    CHECK(ep.state() == EndpointerState::Onset);

    // 32 ms × 6 = 192 ms — still under 200 ms.
    for (int i = 0; i < 5; ++i) {
        CHECK(step(ep, 0.80F, 32ms, t) == FrameOutcome::None);
    }
    // 7th 32 ms frame pushes us over 200 ms.
    CHECK(step(ep, 0.80F, 32ms, t) == FrameOutcome::SpeechStarted);
    CHECK(ep.state() == EndpointerState::Speaking);
}

TEST_CASE("Endpointer: false-start when probability dips below offset before maturing") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    CHECK(step(ep, 0.60F, 32ms, t) == FrameOutcome::None);
    CHECK(ep.state() == EndpointerState::Onset);
    CHECK(step(ep, 0.10F, 32ms, t) == FrameOutcome::FalseStart);
    CHECK(ep.state() == EndpointerState::Quiet);
}

TEST_CASE("Endpointer: SpeechEnded fires after hangover_ms of low probability") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};

    // Get into Speaking.
    while (step(ep, 0.80F, 32ms, t) != FrameOutcome::SpeechStarted) {}
    CHECK(ep.state() == EndpointerState::Speaking);

    // 600 ms / 32 ms ≈ 19 frames of low audio for the endpoint.
    bool ended = false;
    for (int i = 0; i < 25; ++i) {
        if (step(ep, 0.10F, 32ms, t) == FrameOutcome::SpeechEnded) {
            ended = true;
            break;
        }
    }
    CHECK(ended);
    CHECK(ep.state() == EndpointerState::Quiet);
}

TEST_CASE("Endpointer: hangover is reset when speech resumes") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    while (step(ep, 0.80F, 32ms, t) != FrameOutcome::SpeechStarted) {}

    // Half-way through hangover, speech resumes.
    for (int i = 0; i < 8; ++i) {  // 256 ms < hangover_ms 600 ms
        CHECK(step(ep, 0.10F, 32ms, t) == FrameOutcome::None);
    }
    CHECK(ep.state() == EndpointerState::Endpoint);
    CHECK(step(ep, 0.85F, 32ms, t) == FrameOutcome::None);
    CHECK(ep.state() == EndpointerState::Speaking);
}

TEST_CASE("Endpointer: hysteresis band keeps Speaking across small dips") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    while (step(ep, 0.80F, 32ms, t) != FrameOutcome::SpeechStarted) {}
    // 0.40 is between offset (0.35) and onset (0.50) — still Speaking.
    for (int i = 0; i < 30; ++i) {
        CHECK(step(ep, 0.40F, 32ms, t) == FrameOutcome::None);
    }
    CHECK(ep.state() == EndpointerState::Speaking);
}

TEST_CASE("Endpointer: force_endpoint while Speaking yields SpeechEnded") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    while (step(ep, 0.80F, 32ms, t) != FrameOutcome::SpeechStarted) {}
    CHECK(ep.force_endpoint(t) == FrameOutcome::SpeechEnded);
    CHECK(ep.state() == EndpointerState::Quiet);
}

TEST_CASE("Endpointer: force_endpoint in Quiet is a no-op") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    CHECK(ep.force_endpoint(t) == FrameOutcome::None);
}

TEST_CASE("Endpointer: reset clears state and timing") {
    Endpointer ep(default_cfg());
    auto t = std::chrono::steady_clock::time_point{};
    while (step(ep, 0.80F, 32ms, t) != FrameOutcome::SpeechStarted) {}
    ep.reset();
    CHECK(ep.state() == EndpointerState::Quiet);
    CHECK(ep.elapsed_speech() == 0ms);
}
