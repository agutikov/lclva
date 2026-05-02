#include "audio/apm.hpp"

#include "audio/loopback.hpp"
#include "audio/resampler.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <atomic>
#include <cmath>
#include <cstring>

#ifdef ACVA_HAVE_WEBRTC_APM
#include <api/scoped_refptr.h>
#include <modules/audio_processing/include/audio_processing.h>
#endif

namespace acva::audio {

namespace {

// 10 ms is the only frame size webrtc::AudioProcessing accepts.
constexpr int kChunkMs = 10;

constexpr std::size_t samples_per_chunk(int sample_rate_hz) noexcept {
    return static_cast<std::size_t>(sample_rate_hz) *
           static_cast<std::size_t>(kChunkMs) / 1000U;
}

} // namespace

#ifdef ACVA_HAVE_WEBRTC_APM

struct Apm::Impl {
    ApmConfig                                cfg;
    LoopbackSink*                            loopback = nullptr;
    rtc::scoped_refptr<webrtc::AudioProcessing> apm{};
    // Resampler converts the loopback ring's native rate (typically
    // 48 kHz) to the APM's near-rate (16 kHz). Owned + reused across
    // process() calls so soxr's phase tracking carries over chunk
    // boundaries.
    std::unique_ptr<Resampler>               ref_resampler;

    // Pre-sized scratch buffers; sized at construction so process()
    // does not reallocate per call.
    std::vector<std::int16_t>                ref_48k;
    std::vector<std::int16_t>                ref_16k_window;
    std::vector<std::int16_t>                near_out;

    // Latest stats, snapshot in process(). Atomic so /metrics readers
    // can sample without coordination.
    std::atomic<int>                         delay_ms{-1};
    std::atomic<float>                       erle_db{std::nanf("")};
    std::atomic<std::uint64_t>               frames{0};
    bool                                     is_active = false;
};

Apm::Apm(ApmConfig cfg, LoopbackSink* loopback)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg      = cfg;
    impl_->loopback = loopback;

    if (!cfg.aec_enabled && !cfg.ns_enabled && !cfg.agc_enabled) {
        log::info("apm", "all subsystems disabled — running pass-through");
        return;
    }

    impl_->apm = rtc::scoped_refptr<webrtc::AudioProcessing>(
        webrtc::AudioProcessingBuilder().Create());
    if (impl_->apm == nullptr) {
        log::warn("apm",
            "AudioProcessingBuilder().Create() returned null — pass-through");
        return;
    }

    webrtc::AudioProcessing::Config apm_cfg{};
    apm_cfg.echo_canceller.enabled = cfg.aec_enabled;
    apm_cfg.echo_canceller.mobile_mode = false;
    apm_cfg.high_pass_filter.enabled = cfg.aec_enabled;
    apm_cfg.noise_suppression.enabled = cfg.ns_enabled;
    apm_cfg.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
    apm_cfg.gain_controller1.enabled = cfg.agc_enabled;
    apm_cfg.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kFixedDigital;
    apm_cfg.gain_controller1.target_level_dbfs = 3;
    apm_cfg.gain_controller1.compression_gain_db = 9;
    apm_cfg.gain_controller1.enable_limiter = true;
    apm_cfg.residual_echo_detector.enabled = true;
    impl_->apm->ApplyConfig(apm_cfg);

    // Initial delay hint. APM's internal estimator refines from here.
    if (cfg.aec_enabled) {
        impl_->apm->set_stream_delay_ms(cfg.initial_delay_estimate_ms);
    }

    impl_->ref_resampler = std::make_unique<Resampler>(
        static_cast<double>(cfg.reverse_sample_rate_hz),
        static_cast<double>(cfg.near_sample_rate_hz),
        Resampler::Quality::High);

    impl_->ref_48k.assign(samples_per_chunk(cfg.reverse_sample_rate_hz), 0);
    impl_->ref_16k_window.assign(samples_per_chunk(cfg.near_sample_rate_hz), 0);
    impl_->near_out.assign(samples_per_chunk(cfg.near_sample_rate_hz), 0);
    impl_->is_active = true;

    log::info("apm",
        fmt::format("initialised: aec={} ns={} agc={} initial_delay={}ms",
                    cfg.aec_enabled, cfg.ns_enabled, cfg.agc_enabled,
                    cfg.initial_delay_estimate_ms));
}

Apm::~Apm() = default;

Apm::Apm(Apm&&) noexcept            = default;
Apm& Apm::operator=(Apm&&) noexcept = default;

bool Apm::aec_active() const noexcept {
    return impl_ && impl_->is_active && impl_->cfg.aec_enabled;
}

std::vector<std::int16_t>
Apm::process(std::span<const std::int16_t> mic_frame,
              std::chrono::steady_clock::time_point capture_time) {
    impl_->frames.fetch_add(1, std::memory_order_relaxed);

    const std::size_t expected = samples_per_chunk(impl_->cfg.near_sample_rate_hz);
    if (mic_frame.size() != expected || !impl_->is_active) {
        // Pass-through: stub build, AEC disabled, or wrong frame size.
        // Wrong-size frames currently can't happen in production (the
        // pipeline always feeds 10 ms = 160 samples) but we'd rather
        // degrade gracefully than corrupt audio mid-stream.
        return {mic_frame.begin(), mic_frame.end()};
    }

    // Pull the reference-signal window matching `capture_time`. The
    // emit-time anchor in LoopbackSink is "now-ish" by construction
    // (updated on every render_into); requesting at capture_time
    // returns the ref samples whose physical emission lined up with
    // the capture instant. APM's stream-delay hint accounts for the
    // residual speaker→mic air delay.
    const std::size_t ref_in_samples =
        samples_per_chunk(impl_->cfg.reverse_sample_rate_hz);
    if (impl_->loopback != nullptr) {
        impl_->loopback->aligned(
            capture_time,
            std::span<std::int16_t>{impl_->ref_48k.data(), ref_in_samples});
    } else {
        std::memset(impl_->ref_48k.data(), 0,
                     ref_in_samples * sizeof(std::int16_t));
    }

    // Resample 48 kHz → 16 kHz via the persistent soxr instance. We
    // need exactly `expected` samples per call; soxr produces a
    // chunk that may be slightly longer or shorter on the first few
    // calls (warm-up). When short, pad with zeros; when long, take
    // the head. Once warm, output size matches input × ratio
    // exactly.
    auto ref_resampled = impl_->ref_resampler->process(
        std::span<const std::int16_t>{impl_->ref_48k.data(), ref_in_samples});
    if (ref_resampled.size() >= expected) {
        std::memcpy(impl_->ref_16k_window.data(),
                     ref_resampled.data(),
                     expected * sizeof(std::int16_t));
    } else {
        std::memcpy(impl_->ref_16k_window.data(),
                     ref_resampled.data(),
                     ref_resampled.size() * sizeof(std::int16_t));
        std::memset(impl_->ref_16k_window.data() + ref_resampled.size(),
                     0,
                     (expected - ref_resampled.size()) * sizeof(std::int16_t));
    }

    const webrtc::StreamConfig sc(impl_->cfg.near_sample_rate_hz, 1U);

    // Reverse stream first so APM has the reference for cross-correlation.
    impl_->apm->ProcessReverseStream(
        impl_->ref_16k_window.data(), sc, sc, impl_->ref_16k_window.data());

    // Refresh the delay hint each frame. APM uses this as a search
    // window center; its internal estimator is the source of truth
    // and is exposed via GetStatistics().delay_ms.
    if (impl_->cfg.aec_enabled) {
        impl_->apm->set_stream_delay_ms(impl_->cfg.initial_delay_estimate_ms);
    }

    impl_->apm->ProcessStream(
        mic_frame.data(), sc, sc, impl_->near_out.data());

    // Snapshot stats for /metrics readers.
    const auto stats = impl_->apm->GetStatistics();
    if (stats.delay_ms.has_value()) {
        impl_->delay_ms.store(stats.delay_ms.value(),
                               std::memory_order_relaxed);
    }
    if (stats.echo_return_loss_enhancement.has_value()) {
        impl_->erle_db.store(
            static_cast<float>(stats.echo_return_loss_enhancement.value()),
            std::memory_order_relaxed);
    }

    return {impl_->near_out.begin(), impl_->near_out.end()};
}

int Apm::aec_delay_estimate_ms() const noexcept {
    return impl_->delay_ms.load(std::memory_order_relaxed);
}

float Apm::erle_db() const noexcept {
    return impl_->erle_db.load(std::memory_order_relaxed);
}

std::uint64_t Apm::frames_processed() const noexcept {
    return impl_->frames.load(std::memory_order_relaxed);
}

#else  // !ACVA_HAVE_WEBRTC_APM

struct Apm::Impl {
    ApmConfig                  cfg;
    LoopbackSink*              loopback = nullptr;
    std::atomic<std::uint64_t> frames{0};
};

Apm::Apm(ApmConfig cfg, LoopbackSink* loopback)
    : impl_(std::make_unique<Impl>()) {
    impl_->cfg      = cfg;
    impl_->loopback = loopback;
    log::warn("apm",
        "built without webrtc-audio-processing — AEC will be a no-op stub");
}

Apm::~Apm() = default;

Apm::Apm(Apm&&) noexcept            = default;
Apm& Apm::operator=(Apm&&) noexcept = default;

bool Apm::aec_active() const noexcept { return false; }

std::vector<std::int16_t>
Apm::process(std::span<const std::int16_t> mic_frame,
              std::chrono::steady_clock::time_point /*capture_time*/) {
    impl_->frames.fetch_add(1, std::memory_order_relaxed);
    return {mic_frame.begin(), mic_frame.end()};
}

int   Apm::aec_delay_estimate_ms() const noexcept { return -1; }
float Apm::erle_db()               const noexcept { return std::nanf(""); }

std::uint64_t Apm::frames_processed() const noexcept {
    return impl_->frames.load(std::memory_order_relaxed);
}

#endif  // ACVA_HAVE_WEBRTC_APM

} // namespace acva::audio
