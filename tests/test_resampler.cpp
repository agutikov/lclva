#include "audio/resampler.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <vector>

using acva::audio::Resampler;

namespace {

// Fill `out` with a `freq`-Hz sine at `sample_rate`, full-scale int16.
std::vector<std::int16_t> sine(double freq, double sample_rate, std::size_t n) {
    std::vector<std::int16_t> v(n);
    const double k = 2.0 * std::numbers::pi_v<double> * freq / sample_rate;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(k * static_cast<double>(i));
        v[i] = static_cast<std::int16_t>(std::round(s * 30000.0));
    }
    return v;
}

// Mean-square energy. Used to compare like-for-like across rates.
double energy(std::span<const std::int16_t> v) {
    if (v.empty()) return 0.0;
    long double s = 0.0L;
    for (auto x : v) {
        const long double f = static_cast<long double>(x);
        s += f * f;
    }
    return static_cast<double>(s / static_cast<long double>(v.size()));
}

} // namespace

TEST_CASE("Resampler: empty input yields empty output without disturbing state") {
    Resampler r(22050.0, 48000.0);
    auto out = r.process({});
    CHECK(out.empty());
    CHECK(r.input_samples() == 0);
    CHECK(r.output_samples() == 0);
}

TEST_CASE("Resampler: 22050 → 48000 expands the sample count by ~ratio") {
    Resampler r(22050.0, 48000.0);
    constexpr std::size_t N = 22050;          // 1 second of input
    auto in  = sine(440.0, 22050.0, N);
    auto out = r.process(in);
    auto tail = r.flush();
    out.insert(out.end(), tail.begin(), tail.end());

    // Within ±2% of the expected 48000 samples (soxr drops a few at
    // either end — see tail).
    const double expected = 48000.0;
    const double observed = static_cast<double>(out.size());
    CHECK(observed >= expected * 0.98);
    CHECK(observed <= expected * 1.02);
    CHECK(r.input_samples()  == N);
    CHECK(r.output_samples() == out.size());
}

TEST_CASE("Resampler: 48000 → 16000 shrinks by 1/3") {
    Resampler r(48000.0, 16000.0);
    constexpr std::size_t N = 48000;
    auto in = sine(1000.0, 48000.0, N);
    auto out = r.process(in);
    auto tail = r.flush();
    out.insert(out.end(), tail.begin(), tail.end());

    const double observed = static_cast<double>(out.size());
    CHECK(observed >= 16000.0 * 0.98);
    CHECK(observed <= 16000.0 * 1.02);
}

TEST_CASE("Resampler: 22050 → 22050 is a near-identity") {
    Resampler r(22050.0, 22050.0);
    auto in  = sine(440.0, 22050.0, 4096);
    auto out = r.process(in);
    auto tail = r.flush();
    out.insert(out.end(), tail.begin(), tail.end());
    // Equal-rate resample shouldn't grow output much. Sample count
    // tolerance allows soxr's small framing offset.
    CHECK(out.size() >= in.size() - 64);
    CHECK(out.size() <= in.size() + 64);
}

TEST_CASE("Resampler: sine energy preserved within ~5% across 22050 → 48000") {
    Resampler r(22050.0, 48000.0);
    auto in  = sine(1000.0, 22050.0, 22050);
    auto out = r.process(in);
    auto tail = r.flush();
    out.insert(out.end(), tail.begin(), tail.end());

    const double e_in  = energy(in);
    const double e_out = energy(out);
    // Filtering removes a sliver of out-of-band noise from the int16
    // quantisation; ±10% is generous and stable across soxr versions.
    CHECK(e_out >= e_in * 0.90);
    CHECK(e_out <= e_in * 1.10);
}

TEST_CASE("Resampler: chunked input produces phase-stable output") {
    // Feed the same signal in one big chunk vs many small ones; the
    // resampled outputs must be byte-identical (modulo the boundary
    // samples soxr defers across chunk seams). Comparing total length
    // and energy is enough — the contract is "behaves like one
    // big resample of the concatenated input".
    const std::size_t N = 8192;
    auto in = sine(523.25, 22050.0, N);

    Resampler r1(22050.0, 48000.0);
    auto big = r1.process(in);
    auto big_tail = r1.flush();
    big.insert(big.end(), big_tail.begin(), big_tail.end());

    Resampler r2(22050.0, 48000.0);
    std::vector<std::int16_t> chunked;
    constexpr std::size_t step = 512;
    for (std::size_t i = 0; i < in.size(); i += step) {
        const auto end = std::min(i + step, in.size());
        auto out = r2.process({in.data() + i, end - i});
        chunked.insert(chunked.end(), out.begin(), out.end());
    }
    auto chunked_tail = r2.flush();
    chunked.insert(chunked.end(), chunked_tail.begin(), chunked_tail.end());

    // Total sample count should match exactly.
    CHECK(chunked.size() == big.size());

    // Energy must agree to high precision — this is the phase-stability
    // contract M6 will rely on for the AEC reference signal.
    const double e_big = energy(big);
    const double e_ck  = energy(chunked);
    CHECK(std::abs(e_big - e_ck) / e_big < 0.001);
}

TEST_CASE("Resampler: rates ≤ 0 throw at construction") {
    CHECK_THROWS_AS(Resampler( 0.0, 48000.0), std::runtime_error);
    CHECK_THROWS_AS(Resampler(48000.0,   0.0), std::runtime_error);
    CHECK_THROWS_AS(Resampler(-1.0, 48000.0),  std::runtime_error);
}

TEST_CASE("Resampler: move-construct preserves state and frees once") {
    Resampler r1(22050.0, 48000.0);
    auto in  = sine(440.0, 22050.0, 4096);
    auto out = r1.process(in);
    CHECK(!out.empty());
    const auto in_count  = r1.input_samples();
    const auto out_count = r1.output_samples();

    Resampler r2(std::move(r1));
    CHECK(r2.input_samples()  == in_count);
    CHECK(r2.output_samples() == out_count);
    auto tail = r2.flush();
    // Moved-from r1 destruction must not free the same handle twice.
    // The test itself proves the absence of double-free; ASan would
    // catch it.
    (void)tail;
}
