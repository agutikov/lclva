#include "audio/vad.hpp"
#include "config/paths.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <stdexcept>
#include <vector>

using acva::audio::SileroVad;

namespace {

std::vector<std::int16_t> silence(std::size_t n) {
    return std::vector<std::int16_t>(n, 0);
}

std::vector<std::int16_t> tone(std::size_t n, double freq_hz, double sr = 16000.0) {
    std::vector<std::int16_t> out(n);
    const double k = 2.0 * std::numbers::pi_v<double> * freq_hz / sr;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::sin(k * static_cast<double>(i));
        out[i] = static_cast<std::int16_t>(std::round(s * 16000.0));
    }
    return out;
}

// Resolve the Silero model path the same way main.cpp does:
//   1. ACVA_SILERO_MODEL env var (override, mainly for CI tooling)
//   2. ${XDG_DATA_HOME:-$HOME/.local/share}/acva/models/silero/silero_vad.onnx
//      — written by scripts/download-vad.sh.
//   3. (legacy fallback) the same path without the silero/ subfolder,
//      for dev environments that haven't run the migration yet.
//
// Returns empty when nothing exists; the gated tests then skip. The
// dev workstation has the model in the XDG location so no env var is
// needed there.
std::filesystem::path model_path_or_empty() {
    if (const char* p = std::getenv("ACVA_SILERO_MODEL"); p && *p) {
        std::filesystem::path candidate{p};
        if (std::filesystem::exists(candidate)) return candidate;
    }
    for (const auto* rel : {"models/silero/silero_vad.onnx",
                              "models/silero_vad.onnx"}) {
        auto resolved = acva::config::resolve_data_path("", rel);
        if (std::filesystem::exists(resolved)) return resolved;
    }
    return {};
}

} // namespace

TEST_CASE("SileroVad: missing model file throws") {
    CHECK_THROWS_AS(SileroVad("/nonexistent/silero.onnx"), std::runtime_error);
}

TEST_CASE("SileroVad: silence yields a low probability"
          * doctest::skip(model_path_or_empty().empty())) {
    SileroVad vad(model_path_or_empty());
    auto buf = silence(16000);
    float last_p = 0.0F;
    constexpr std::size_t step = 512;
    for (std::size_t i = 0; i < buf.size(); i += step) {
        last_p = vad.push_frame({buf.data() + i,
                                  std::min(step, buf.size() - i)});
    }
    CHECK(last_p < 0.2F);
}

TEST_CASE("SileroVad: a 200 Hz pure tone is not "
          "(necessarily) flagged as speech, but the call doesn't crash"
          * doctest::skip(model_path_or_empty().empty())) {
    // Silero's training distribution doesn't include pure tones; the
    // model can return a wide range of probabilities. The test only
    // asserts the call returns a valid number in [0, 1].
    SileroVad vad(model_path_or_empty());
    auto buf = tone(8192, 200.0);
    const float p = vad.push_frame(buf);
    CHECK(p >= 0.0F);
    CHECK(p <= 1.0F);
}

TEST_CASE("SileroVad: reset clears state"
          * doctest::skip(model_path_or_empty().empty())) {
    SileroVad vad(model_path_or_empty());
    auto buf = silence(2048);
    vad.push_frame(buf);
    vad.reset();
    // After reset, the model should still produce output without
    // crashing.
    const float p = vad.push_frame(buf);
    CHECK(p >= 0.0F);
    CHECK(p <= 1.0F);
}
