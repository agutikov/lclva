#include "audio/apm.hpp"
#include "audio/loopback.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using acva::audio::Apm;
using acva::audio::ApmConfig;
using acva::audio::LoopbackSink;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t k10msAt16k = 160;
constexpr std::size_t k10msAt48k = 480;

const auto kT0 = std::chrono::steady_clock::time_point{} +
                  std::chrono::seconds(1000);

std::vector<std::int16_t> tone(double freq_hz, double rate_hz,
                                std::size_t n, std::int16_t amp = 16000) {
    std::vector<std::int16_t> v(n);
    constexpr double kTau = 6.283185307179586;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(kTau * freq_hz * static_cast<double>(i) / rate_hz);
        v[i] = static_cast<std::int16_t>(s * amp);
    }
    return v;
}

} // namespace

TEST_CASE("Apm: pass-through when all subsystems disabled") {
    ApmConfig cfg{};
    cfg.aec_enabled = false;
    cfg.ns_enabled = false;
    cfg.agc_enabled = false;

    Apm apm(cfg, /*loopback=*/nullptr);
    REQUIRE_FALSE(apm.aec_active());

    const auto in = tone(/*freq=*/440.0, /*rate=*/16000.0, k10msAt16k);
    auto out = apm.process(in, kT0);

    REQUIRE(out.size() == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == in[i]);
    }
    REQUIRE(apm.frames_processed() == 1);
}

TEST_CASE("Apm: wrong-size frames pass through") {
    ApmConfig cfg{};
    Apm apm(cfg, /*loopback=*/nullptr);

    // 320 samples = 20 ms; APM only takes 10 ms = 160 samples. Wrapper
    // returns the input verbatim instead of corrupting the stream.
    std::vector<std::int16_t> in(k10msAt16k * 2, std::int16_t{1234});
    auto out = apm.process(in, kT0);
    REQUIRE(out.size() == in.size());
    for (auto x : out) {
        REQUIRE(x == 1234);
    }
}

TEST_CASE("Apm: frames_processed counts every call") {
    ApmConfig cfg{};
    Apm apm(cfg, /*loopback=*/nullptr);

    std::vector<std::int16_t> in(k10msAt16k, 0);
    for (int i = 0; i < 5; ++i) {
        (void)apm.process(in, kT0 + std::chrono::milliseconds(i * 10));
    }
    REQUIRE(apm.frames_processed() == 5);
}

TEST_CASE("Apm: process returns 10 ms output for 10 ms input") {
    ApmConfig cfg{};      // defaults: aec+ns+agc all on
    LoopbackSink loopback(/*capacity=*/96000, /*rate=*/48000);

    Apm apm(cfg, &loopback);

    // Whether webrtc-audio-processing-1 is linked or not, the wrapper
    // must hand back a frame of the requested length.
    std::vector<std::int16_t> mic(k10msAt16k, 0);
    auto out = apm.process(mic, kT0);
    REQUIRE(out.size() == k10msAt16k);
}

TEST_CASE("Apm: ref-signal silence yields cleaned mic close to mic") {
    // With NS off + AGC off + AEC on, and SILENT loopback ref, APM has
    // nothing to cancel — the cleaned mic should look essentially like
    // the input. We verify shape rather than bit-exactness because APM
    // applies a high-pass filter that nudges low-frequency content.
    ApmConfig cfg{};
    cfg.ns_enabled = false;
    cfg.agc_enabled = false;
    LoopbackSink loopback(/*capacity=*/48000, /*rate=*/48000);

    Apm apm(cfg, &loopback);

    // Run 1 s worth of frames so the APM warm-up converges.
    auto mic = tone(440.0, 16000.0, k10msAt16k);
    long total_in_energy  = 0;
    long total_out_energy = 0;
    for (int i = 0; i < 100; ++i) {
        auto out = apm.process(mic, kT0 + std::chrono::milliseconds(i * 10));
        REQUIRE(out.size() == k10msAt16k);
        for (std::size_t j = 0; j < mic.size(); ++j) {
            total_in_energy  += std::abs(mic[j]);
            total_out_energy += std::abs(out[j]);
        }
    }
    // Output energy should be at least 50% of input — APM's HPF + AEC
    // removes some low-band content but a 440 Hz pure tone survives.
    if (apm.aec_active()) {
        REQUIRE(total_out_energy > total_in_energy / 2);
    }
}

TEST_CASE("Apm: loopback emits ref, APM consumes without crashing") {
    ApmConfig cfg{};
    LoopbackSink loopback(/*capacity=*/48000, /*rate=*/48000);
    Apm apm(cfg, &loopback);

    // Feed loopback a tone, then process several mic frames. The mic
    // is silent so AEC should converge to near-silence output. We
    // don't assert on cancellation depth here — that lives in the
    // gated echo-validation test (M6 Step 5). What we assert is that
    // the wiring runs without exception or memory error and that
    // frames_processed advances.
    const auto ref = tone(800.0, 48000.0, k10msAt48k);
    std::vector<std::int16_t> mic_silence(k10msAt16k, 0);

    for (int i = 0; i < 10; ++i) {
        const auto t = kT0 + std::chrono::milliseconds(i * 10);
        loopback.on_emitted(ref, t);
        (void)apm.process(mic_silence, t);
    }
    REQUIRE(apm.frames_processed() == 10);
}
