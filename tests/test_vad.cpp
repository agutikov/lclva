#include "audio/vad.hpp"

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

// VAD model path is opt-in via env var. Without it the gated tests
// below skip — the build environment doesn't ship the model file.
std::filesystem::path model_path_or_empty() {
    if (const char* p = std::getenv("ACVA_SILERO_MODEL")) {
        return std::filesystem::path{p};
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
