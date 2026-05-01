#include "audio/vad.hpp"

#include <stdexcept>
#include <string>

#ifdef ACVA_HAVE_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#endif

namespace acva::audio {

#ifdef ACVA_HAVE_ONNXRUNTIME

namespace {

// Silero VAD v5 model expects 512 samples at 16 kHz (32 ms) and
// 256 samples at 8 kHz. We hard-code 16 kHz here — that's the rate
// the M4 capture path lands at after the 48 → 16 kHz resample.
constexpr std::size_t kWindowSamples16k = 512;
constexpr std::size_t kStateSize        = 2ULL * 1 * 128;

} // namespace

struct SileroVad::Impl {
    Ort::Env       env{ORT_LOGGING_LEVEL_WARNING, "acva-vad"};
    Ort::SessionOptions session_opts{};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::MemoryInfo memory_info{nullptr};

    std::vector<std::string> input_names_str;
    std::vector<std::string> output_names_str;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;

    std::array<float, kStateSize> state{};
    std::array<std::int64_t, 1>   sr_value{16000};

    std::vector<float> scratch;
    float last_p = 0.0F;

    Impl() {
        memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        session_opts.SetIntraOpNumThreads(1);
        session_opts.SetInterOpNumThreads(1);
        session_opts.SetGraphOptimizationLevel(
            GraphOptimizationLevel::ORT_ENABLE_BASIC);
    }

    void load(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("vad: model file not found: " + path.string());
        }
        try {
            session = std::make_unique<Ort::Session>(
                env, path.c_str(), session_opts);
        } catch (const Ort::Exception& ex) {
            throw std::runtime_error(
                std::string{"vad: failed to load ONNX model: "} + ex.what());
        }

        const std::size_t n_in  = session->GetInputCount();
        const std::size_t n_out = session->GetOutputCount();
        input_names_str.reserve(n_in);
        output_names_str.reserve(n_out);
        for (std::size_t i = 0; i < n_in; ++i) {
            auto name = session->GetInputNameAllocated(i, allocator);
            input_names_str.emplace_back(name.get());
        }
        for (std::size_t i = 0; i < n_out; ++i) {
            auto name = session->GetOutputNameAllocated(i, allocator);
            output_names_str.emplace_back(name.get());
        }
        input_names.reserve(n_in);
        output_names.reserve(n_out);
        for (auto& s : input_names_str)  input_names.push_back(s.c_str());
        for (auto& s : output_names_str) output_names.push_back(s.c_str());

        if (n_in != 3) {
            throw std::runtime_error(
                "vad: unsupported model — expected 3 inputs (Silero v5), got "
                + std::to_string(n_in));
        }
    }

    float run() {
        const std::array<std::int64_t, 2> input_shape{1,
            static_cast<std::int64_t>(kWindowSamples16k)};
        const std::array<std::int64_t, 3> state_shape{2, 1, 128};
        const std::array<std::int64_t, 1> sr_shape{1};

        std::array<Ort::Value, 3> inputs{
            Ort::Value::CreateTensor<float>(memory_info,
                scratch.data(), kWindowSamples16k,
                input_shape.data(), input_shape.size()),
            Ort::Value::CreateTensor<float>(memory_info,
                state.data(), state.size(),
                state_shape.data(), state_shape.size()),
            Ort::Value::CreateTensor<std::int64_t>(memory_info,
                sr_value.data(), sr_value.size(),
                sr_shape.data(), sr_shape.size()),
        };

        auto outputs = session->Run(Ort::RunOptions{nullptr},
            input_names.data(),  inputs.data(),  inputs.size(),
            output_names.data(), output_names.size());

        const float* p = outputs[0].GetTensorData<float>();
        last_p = p[0];

        const float* next_state = outputs[1].GetTensorData<float>();
        std::memcpy(state.data(), next_state, kStateSize * sizeof(float));

        return last_p;
    }
};

SileroVad::SileroVad(const std::filesystem::path& model_path,
                      std::uint32_t sample_rate)
    : impl_(std::make_unique<Impl>()),
      sample_rate_(sample_rate) {
    if (sample_rate != 16000) {
        throw std::runtime_error(
            "vad: only 16 kHz is supported (got "
            + std::to_string(sample_rate) + ")");
    }
    impl_->load(model_path);
    impl_->scratch.reserve(kWindowSamples16k * 2);
}

SileroVad::~SileroVad() = default;
SileroVad::SileroVad(SileroVad&&) noexcept = default;
SileroVad& SileroVad::operator=(SileroVad&&) noexcept = default;

std::size_t SileroVad::window_samples() const noexcept {
    return kWindowSamples16k;
}

void SileroVad::reset() {
    if (!impl_) return;
    impl_->state.fill(0.0F);
    impl_->scratch.clear();
    impl_->last_p = 0.0F;
}

float SileroVad::push_frame(std::span<const std::int16_t> samples) {
    if (samples.empty() || !impl_) return impl_ ? impl_->last_p : 0.0F;

    auto& s = impl_->scratch;
    const std::size_t prev = s.size();
    s.resize(prev + samples.size());
    constexpr float kInv = 1.0F / 32768.0F;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        s[prev + i] = static_cast<float>(samples[i]) * kInv;
    }

    while (s.size() >= kWindowSamples16k) {
        impl_->run();
        s.erase(s.begin(),
                s.begin() + static_cast<std::ptrdiff_t>(kWindowSamples16k));
    }
    return impl_->last_p;
}

#else // !ACVA_HAVE_ONNXRUNTIME

// Stub when ONNX Runtime is not installed. Construction throws
// unconditionally; the M4 audio pipeline catches this and downgrades
// to "no VAD" mode.
struct SileroVad::Impl {};

SileroVad::SileroVad(const std::filesystem::path& /*model_path*/,
                      std::uint32_t /*sample_rate*/) {
    throw std::runtime_error(
        "vad: built without ONNX Runtime — Silero VAD unavailable");
}

SileroVad::~SileroVad() = default;
SileroVad::SileroVad(SileroVad&&) noexcept = default;
SileroVad& SileroVad::operator=(SileroVad&&) noexcept = default;

std::size_t SileroVad::window_samples() const noexcept { return 512; }
void SileroVad::reset() {}
float SileroVad::push_frame(std::span<const std::int16_t>) { return 0.0F; }

#endif // ACVA_HAVE_ONNXRUNTIME

} // namespace acva::audio
