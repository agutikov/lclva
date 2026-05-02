#include "audio/loopback.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using acva::audio::LoopbackSink;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kSr = 48000;

// Fixed reference instant so test math stays in integer-ns territory.
const auto kT0 = std::chrono::steady_clock::time_point{} +
                  std::chrono::seconds(1000);

// `n` samples whose values are an arithmetic ramp starting at `start`.
std::vector<std::int16_t> ramp(std::int16_t start, std::size_t n) {
    std::vector<std::int16_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::int16_t>(start + static_cast<int>(i));
    }
    return v;
}

} // namespace

TEST_CASE("LoopbackSink: empty until first emit") {
    LoopbackSink s(48000, kSr);

    REQUIRE_FALSE(s.has_data());
    REQUIRE(s.total_frames_emitted() == 0);

    auto pulled = s.aligned(kT0, 160);
    REQUIRE(pulled.size() == 160);
    for (auto x : pulled) {
        REQUIRE(x == 0);
    }
}

TEST_CASE("LoopbackSink: aligned() returns the exact written window") {
    LoopbackSink s(48000, kSr);

    // Emit 480 samples (a 10 ms chunk at 48 kHz) at t=kT0.
    const auto chunk = ramp(/*start=*/100, /*n=*/480);
    s.on_emitted(chunk, kT0);
    REQUIRE(s.has_data());
    REQUIRE(s.total_frames_emitted() == 480);

    // Pulling at exactly the emit time should return the chunk verbatim.
    auto got = s.aligned(kT0, 480);
    REQUIRE(got.size() == 480);
    for (std::size_t i = 0; i < got.size(); ++i) {
        REQUIRE(got[i] == chunk[i]);
    }
}

TEST_CASE("LoopbackSink: aligned() shifts by the requested capture time") {
    LoopbackSink s(48000, kSr);

    // Two consecutive 480-sample chunks; the second starts 10 ms after
    // the first. last_emit_first_frame_ tracks the first frame of the
    // most recent chunk = 480.
    const auto a = ramp(0,    480);
    const auto b = ramp(1000, 480);
    s.on_emitted(a, kT0);
    s.on_emitted(b, kT0 + 10ms);

    // Asking for 480 samples at kT0 must hit the first chunk.
    auto first = s.aligned(kT0, 480);
    REQUIRE(first.size() == 480);
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(first[i] == a[i]);
    }

    // Asking 10 ms later returns the second chunk.
    auto second = s.aligned(kT0 + 10ms, 480);
    REQUIRE(second.size() == 480);
    for (std::size_t i = 0; i < second.size(); ++i) {
        REQUIRE(second[i] == b[i]);
    }

    // A 5 ms shift straddles the boundary: 240 samples from the tail
    // of `a`, then 240 from the head of `b`.
    auto straddle = s.aligned(kT0 + 5ms, 480);
    REQUIRE(straddle.size() == 480);
    for (std::size_t i = 0; i < 240; ++i) {
        REQUIRE(straddle[i] == a[i + 240]);
    }
    for (std::size_t i = 0; i < 240; ++i) {
        REQUIRE(straddle[240 + i] == b[i]);
    }
}

TEST_CASE("LoopbackSink: future capture_time zero-pads") {
    LoopbackSink s(48000, kSr);
    s.on_emitted(ramp(7, 480), kT0);

    // Ask for samples 100 ms in the future of the most recent emit —
    // we don't have those frames yet, so the entire window is zero.
    auto out = s.aligned(kT0 + 100ms, 480);
    for (auto x : out) {
        REQUIRE(x == 0);
    }
}

TEST_CASE("LoopbackSink: aged-out capture_time zero-pads") {
    // Capacity intentionally smaller than the data we'll write so the
    // oldest frames fall off.
    LoopbackSink s(/*capacity_samples=*/960, kSr);

    // Three back-to-back 480-sample chunks. After the third, the ring
    // holds only frames [480, 1440); frames [0, 480) have aged out.
    s.on_emitted(ramp(0, 480),    kT0);
    s.on_emitted(ramp(1000, 480), kT0 + 10ms);
    s.on_emitted(ramp(2000, 480), kT0 + 20ms);
    REQUIRE(s.total_frames_emitted() == 1440);

    // Pulling at kT0 (which lines up with the now-evicted first chunk)
    // returns silence.
    auto stale = s.aligned(kT0, 480);
    for (auto x : stale) {
        REQUIRE(x == 0);
    }

    // The most recent chunk is still retrievable.
    auto fresh = s.aligned(kT0 + 20ms, 480);
    REQUIRE(fresh[0]   == 2000);
    REQUIRE(fresh[479] == 2000 + 479);
}

TEST_CASE("LoopbackSink: writes larger than capacity keep the tail") {
    LoopbackSink s(/*capacity_samples=*/480, kSr);

    // Push 1200 samples in one shot. Only the last 480 are retained.
    auto big = ramp(0, 1200);
    s.on_emitted(big, kT0);
    REQUIRE(s.total_frames_emitted() == 1200);

    // The retained tail is frames [720, 1200). last_emit_first_frame_
    // tracks 720, with last_emit_time_ = kT0. Pulling at the time
    // corresponding to frame 720 returns the tail.
    const auto tail_time = kT0;  // first sample of the retained tail
    auto out = s.aligned(tail_time, 480);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i] == big[720 + i]);
    }
}

TEST_CASE("LoopbackSink: clear() resets state") {
    LoopbackSink s(48000, kSr);
    s.on_emitted(ramp(0, 480), kT0);
    REQUIRE(s.has_data());

    s.clear();
    REQUIRE_FALSE(s.has_data());
    REQUIRE(s.total_frames_emitted() == 0);

    auto out = s.aligned(kT0, 480);
    for (auto x : out) {
        REQUIRE(x == 0);
    }
}

TEST_CASE("LoopbackSink: ring wraparound preserves samples in order") {
    // Capacity 1440 samples (30 ms), write 4 chunks of 480 (=1920
    // samples) so the ring wraps once. Frames [480, 1920) are retained.
    LoopbackSink s(1440, kSr);

    for (int chunk = 0; chunk < 4; ++chunk) {
        const auto data = ramp(static_cast<std::int16_t>(chunk * 1000), 480);
        s.on_emitted(data, kT0 + std::chrono::milliseconds(chunk * 10));
    }
    REQUIRE(s.total_frames_emitted() == 1920);

    // Pull the last 1440 samples — should match chunks 1, 2, 3 in order.
    auto whole_window = s.aligned(kT0 + 10ms, 1440);
    for (std::size_t chunk = 1; chunk < 4; ++chunk) {
        const auto base = static_cast<std::int16_t>(chunk * 1000);
        for (std::size_t i = 0; i < 480; ++i) {
            const std::size_t idx = (chunk - 1) * 480 + i;
            REQUIRE(whole_window[idx]
                    == static_cast<std::int16_t>(base + static_cast<int>(i)));
        }
    }
}
