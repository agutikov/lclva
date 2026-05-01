#include "audio/utterance.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <vector>

using acva::audio::UtteranceBuffer;
using namespace std::chrono_literals;

namespace {

std::vector<std::int16_t> ramp(std::size_t n, std::int16_t base = 0) {
    std::vector<std::int16_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::int16_t>(base + static_cast<std::int16_t>(i & 0xFFFF));
    }
    return v;
}

} // namespace

TEST_CASE("UtteranceBuffer: rolling pre-buffer is bounded by pre_padding_ms") {
    UtteranceBuffer buf(16000, 300ms, 100ms);
    // Append 500 ms of audio while idle. Expect pre-buffer to keep
    // the most recent 300 ms (4800 samples).
    auto data = ramp(8000);
    buf.append(data);
    CHECK(buf.pre_buffer_samples() == 4800);
}

TEST_CASE("UtteranceBuffer: SpeechStarted adopts the rolling pre-buffer") {
    UtteranceBuffer buf(16000, 300ms, 100ms);
    buf.append(ramp(4800));
    CHECK(buf.pre_buffer_samples() == 4800);
    auto t0 = std::chrono::steady_clock::time_point{};
    buf.on_speech_started(t0, t0 + 300ms);
    // Pre-buffer is now drained (lives in the active buffer).
    CHECK(buf.pre_buffer_samples() == 0);
    CHECK(buf.active());
}

TEST_CASE("UtteranceBuffer: on_speech_ended produces an AudioSlice "
           "with samples from pre-pad through end") {
    UtteranceBuffer buf(16000, 300ms, 100ms);
    auto t0 = std::chrono::steady_clock::time_point{};

    buf.append(ramp(4800));            // 300 ms pre-pad
    buf.on_speech_started(t0, t0 + 300ms);
    buf.append(ramp(16000, /*base=*/4800));   // 1 s of "speech"
    auto slice = buf.on_speech_ended(t0 + 1300ms);
    REQUIRE(slice);
    // Expect ≈ 1300 ms + 100 ms post-pad = 1400 ms ⇒ 22400 samples.
    // We appended 300 ms + 1000 ms = 1300 ms, so the slice ends
    // after 1300 ms (no more samples available beyond what was
    // appended). Actual duration matches what's been buffered.
    CHECK(slice->sample_rate() == 16000);
    CHECK(slice->samples().size() == 4800 + 16000);
}

TEST_CASE("UtteranceBuffer: shared_ptr semantics — slice survives buffer reuse") {
    UtteranceBuffer buf(16000, 100ms, 100ms);
    auto t0 = std::chrono::steady_clock::time_point{};

    buf.on_speech_started(t0, t0);
    buf.append(ramp(8000));
    auto slice1 = buf.on_speech_ended(t0 + 500ms);
    REQUIRE(slice1);
    const std::size_t s1_size = slice1->samples().size();

    buf.on_speech_started(t0 + 1s, t0 + 1s);
    buf.append(ramp(4000));
    auto slice2 = buf.on_speech_ended(t0 + 1300ms);
    REQUIRE(slice2);

    // First slice must still be intact.
    CHECK(slice1->samples().size() == s1_size);
}

TEST_CASE("UtteranceBuffer: max_in_flight enforced by drops counter") {
    UtteranceBuffer buf(16000, 100ms, 100ms, /*max_in_flight=*/2);
    auto t0 = std::chrono::steady_clock::time_point{};

    auto make_slice = [&](int i) {
        buf.on_speech_started(t0 + std::chrono::milliseconds{i * 1000},
                                t0 + std::chrono::milliseconds{i * 1000});
        buf.append(ramp(8000));
        return buf.on_speech_ended(
            t0 + std::chrono::milliseconds{i * 1000 + 500});
    };

    auto a = make_slice(0);
    auto b = make_slice(1);
    auto c = make_slice(2);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);

    CHECK(buf.drops() == 1);
    // All three slices remain valid for their owners.
    CHECK(a->samples().size() == 8000);
    CHECK(b->samples().size() == 8000);
    CHECK(c->samples().size() == 8000);
}

TEST_CASE("UtteranceBuffer: in_flight count drops when consumer releases slice") {
    UtteranceBuffer buf(16000, 100ms, 100ms, 4);
    auto t0 = std::chrono::steady_clock::time_point{};

    buf.on_speech_started(t0, t0);
    buf.append(ramp(8000));
    {
        auto slice = buf.on_speech_ended(t0 + 500ms);
        REQUIRE(slice);
        CHECK(buf.in_flight() == 1);
    }
    CHECK(buf.in_flight() == 0);
}

TEST_CASE("UtteranceBuffer: abort_active discards working buffer") {
    UtteranceBuffer buf(16000, 100ms, 100ms);
    auto t0 = std::chrono::steady_clock::time_point{};
    buf.on_speech_started(t0, t0);
    buf.append(ramp(8000));
    buf.abort_active();
    CHECK(!buf.active());
    auto slice = buf.on_speech_ended(t0 + 500ms);
    CHECK_FALSE(slice);
}

TEST_CASE("UtteranceBuffer: max_duration_ms truncates monologue from the head") {
    UtteranceBuffer buf(16000, 100ms, 100ms, 3, /*max_duration_ms=*/500ms);
    auto t0 = std::chrono::steady_clock::time_point{};
    buf.on_speech_started(t0, t0);
    // Append 1 s of audio — 16000 samples, but cap is 8000.
    for (int i = 0; i < 4; ++i) {
        buf.append(ramp(4000));
    }
    auto slice = buf.on_speech_ended(t0 + 1s);
    REQUIRE(slice);
    // Slice may end up trimmed by post_padding logic too — the cap
    // ensures we never carry > 500 ms = 8000 samples in the active
    // buffer, so the resulting slice is also ≤ 8000 samples.
    CHECK(slice->samples().size() <= 8000);
}
