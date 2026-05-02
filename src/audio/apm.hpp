#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace acva::audio {

class LoopbackSink;

// Configuration for the WebRTC Audio Processing Module wrapper.
// Defaults match `cfg.apm.*` in config/default.yaml.
struct ApmConfig {
    bool aec_enabled = true;
    bool ns_enabled = true;
    bool agc_enabled = true;

    // Initial guess passed to webrtc::AudioProcessing::set_stream_delay_ms.
    // The APM's internal delay estimator refines this over the first
    // few seconds; the initial value just narrows the cross-correlation
    // search window. 50 ms is sane for desktop speakers + USB mic.
    int initial_delay_estimate_ms = 50;

    // Upper bound on the delay APM is willing to estimate. If the
    // estimate climbs above this we log + clamp. Mostly a sanity guard
    // against runaway drift.
    int max_delay_ms = 250;

    // Sample rate of the int16 mono frames passed to process(). APM's
    // ProcessStream/ProcessReverseStream handle rate conversion
    // internally; we pin the wrapper to 16 kHz to match the rate VAD
    // and the M5 STT path expect downstream.
    int near_sample_rate_hz = 16000;

    // Sample rate of the reference samples stored in the LoopbackSink.
    // The wrapper resamples reverse-stream input from this rate to
    // `near_sample_rate_hz` before feeding APM.
    int reverse_sample_rate_hz = 48000;
};

// Wraps webrtc::AudioProcessing and the loopback alignment math behind
// a 10-ms-frame call signature. Lives on the audio-processing worker
// thread (NOT the audio callback) — instantiation allocates and
// process() may allocate via the resampler.
//
// Compiled in two flavors gated on ACVA_HAVE_WEBRTC_APM. When the
// system package is missing, every method short-circuits: process()
// returns the mic frame unchanged and the metrics return their default
// "uninitialized" values. The owning AudioPipeline then operates as if
// AEC were simply disabled — the dialogue path still works but echo is
// not cancelled. This mirrors the M4 Silero-VAD stub strategy.
//
// Thread-safety: not threadsafe. One Apm per consumer (the audio
// pipeline worker is the only consumer in production).
class Apm {
public:
    // `loopback` may be null. When null, every reverse-stream pull
    // returns zeros (silence); APM still runs but has no echo to
    // cancel. Useful for tests that exercise the wrapper without
    // wiring up the playback path.
    Apm(ApmConfig cfg, LoopbackSink* loopback);
    ~Apm();

    Apm(const Apm&)            = delete;
    Apm& operator=(const Apm&) = delete;
    Apm(Apm&&)                 noexcept;
    Apm& operator=(Apm&&)      noexcept;

    // True when the underlying webrtc::AudioProcessing instance was
    // constructed successfully. False when ACVA_HAVE_WEBRTC_APM is
    // undefined or APM creation failed at runtime.
    [[nodiscard]] bool aec_active() const noexcept;

    // Process exactly one 10 ms mic frame at near_sample_rate_hz
    // (i.e., 160 samples at 16 kHz). `capture_time` is the wall-clock
    // instant the FIRST mic sample was captured — used to pull the
    // matching reference window from LoopbackSink. Returns the
    // cleaned frame (same length as input). When AEC is disabled
    // (stub build, AEC turned off in cfg, or construction failed),
    // the input is returned verbatim.
    [[nodiscard]] std::vector<std::int16_t>
    process(std::span<const std::int16_t> mic_frame,
            std::chrono::steady_clock::time_point capture_time);

    // The APM's last-reported instantaneous delay estimate in ms.
    // Returns -1 before any frame has been processed or in the stub
    // build. Mirrored on /metrics as voice_aec_delay_estimate_ms.
    [[nodiscard]] int aec_delay_estimate_ms() const noexcept;

    // The APM's last-reported echo return loss enhancement, in dB.
    // Returns NaN before convergence or in the stub build. Mirrored on
    // /metrics; the M6 acceptance gate is > 25 dB on the validation
    // fixture.
    [[nodiscard]] float erle_db() const noexcept;

    // Total frames processed since construction (counts mic frames
    // passed in regardless of whether APM was actually invoked). Used
    // by tests + metrics.
    [[nodiscard]] std::uint64_t frames_processed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acva::audio
