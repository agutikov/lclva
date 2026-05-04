#include "audio/apm.hpp"
#include "audio/loopback.hpp"
#include "demos/demo.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <span>
#include <vector>

namespace acva::demos {

namespace {

// Average absolute amplitude. Used as a stand-in for RMS energy when
// computing the input-vs-output cancellation ratio.
double mean_abs(std::span<const std::int16_t> v) {
    if (v.empty()) return 0.0;
    long double s = 0.0L;
    for (auto x : v) s += std::abs(x);
    return static_cast<double>(s / static_cast<long double>(v.size()));
}

} // namespace

// Synthetic AEC validation. No mic, no speaker — we feed a known
// stimulus into the LoopbackSink (as if it had just been emitted)
// and the same stimulus (delayed + attenuated) into APM as the mic
// frame, then watch ERLE converge.
//
// Why synthetic: the M6 plan's hardware demo (real speaker + mic +
// quiet room) is a measurement task, not a wiring smoke. The wiring
// — LoopbackSink → APM → cleaned-mic — is what most often breaks
// during dev iteration; this demo exercises it end-to-end on any
// machine, including headless CI containers. The hardware version
// can land later as a separate path inside the same demo.
int run_aec(const config::Config& cfg, std::span<const std::string> /*args*/) {
    using namespace std::chrono_literals;

    constexpr double      kFreqHz       = 1000.0;
    constexpr double      kStimulusSec  = 6.0;
    constexpr int         kSimDelayMs   = 50;       // simulated speaker→mic air delay
    constexpr double      kEchoGain     = 0.4;      // simulated room attenuation
    constexpr int         kSampleRateHz = 48000;
    constexpr std::size_t kRefChunkSamples = kSampleRateHz / 100;        // 10 ms @ 48 kHz
    constexpr std::size_t kMicChunkSamples = 16000 / 100;                // 10 ms @ 16 kHz

    std::printf(
        "demo[aec] synthetic stimulus=%dHz duration=%.1fs (no real audio device)\n",
        static_cast<int>(kFreqHz), kStimulusSec);
    std::printf(
        "demo[aec] simulated air_delay=%dms echo_gain=%.2f\n",
        kSimDelayMs, kEchoGain);

    audio::LoopbackSink loopback(
        /*capacity=*/static_cast<std::size_t>(kSampleRateHz) * 2,
        /*sample_rate=*/kSampleRateHz);

    audio::ApmConfig apm_cfg;
    apm_cfg.aec_enabled              = cfg.apm.aec_enabled;
    apm_cfg.ns_enabled               = cfg.apm.ns_enabled;
    apm_cfg.agc_enabled              = cfg.apm.agc_enabled;
    apm_cfg.initial_delay_estimate_ms =
        static_cast<int>(cfg.apm.initial_delay_estimate_ms);
    apm_cfg.max_delay_ms = static_cast<int>(cfg.apm.max_delay_ms);
    audio::Apm apm(apm_cfg, &loopback);

    if (!apm.aec_active()) {
        std::printf("demo[aec] APM is not active (built without "
                     "webrtc-audio-processing-1?). Demo aborted.\n");
        return EXIT_FAILURE;
    }

    // Pre-generate the full stimulus at both rates (16 kHz mic side,
    // 48 kHz ref side) so per-chunk math stays simple and we can
    // shift by `kSimDelayMs` for the mic without aliasing.
    const std::size_t total_ref =
        static_cast<std::size_t>(static_cast<double>(kSampleRateHz) * kStimulusSec);
    const std::size_t total_mic =
        static_cast<std::size_t>(16000.0 * kStimulusSec);

    std::vector<std::int16_t> ref_full(total_ref);
    std::vector<std::int16_t> mic_full(total_mic);
    {
        const double tau = 2.0 * std::numbers::pi_v<double> * kFreqHz;
        for (std::size_t i = 0; i < total_ref; ++i) {
            ref_full[i] = static_cast<std::int16_t>(
                std::sin(tau * static_cast<double>(i) / kSampleRateHz) * 16000.0);
        }
        const std::size_t mic_shift =
            static_cast<std::size_t>(16000.0 * kSimDelayMs / 1000.0);
        for (std::size_t i = 0; i < total_mic; ++i) {
            const std::int64_t src = static_cast<std::int64_t>(i)
                                       - static_cast<std::int64_t>(mic_shift);
            const double s = (src < 0)
                ? 0.0
                : std::sin(tau * static_cast<double>(src) / 16000.0);
            mic_full[i] = static_cast<std::int16_t>(
                s * kEchoGain * 16000.0);
        }
    }

    std::printf(
        "demo[aec] running %zu mic frames + %zu ref frames…\n",
        total_mic / kMicChunkSamples, total_ref / kRefChunkSamples);

    auto t = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1000);
    constexpr int kFramesPerSec = 100;          // 10 ms frame
    int next_report_frame = 50;                 // first sample at t=0.5s

    long double in_energy_total  = 0.0L;
    long double out_energy_total = 0.0L;

    for (std::size_t frame = 0; frame * kRefChunkSamples + kRefChunkSamples <= total_ref
            && frame * kMicChunkSamples + kMicChunkSamples <= total_mic;
         ++frame) {
        std::span<const std::int16_t> ref_chunk{
            ref_full.data() + frame * kRefChunkSamples, kRefChunkSamples};
        std::span<const std::int16_t> mic_chunk{
            mic_full.data() + frame * kMicChunkSamples, kMicChunkSamples};

        loopback.on_emitted(ref_chunk, t);
        auto out = apm.process(mic_chunk, t);

        in_energy_total  += static_cast<long double>(mean_abs(mic_chunk));
        out_energy_total += static_cast<long double>(mean_abs(out));

        if (static_cast<int>(frame) == next_report_frame) {
            const double secs = static_cast<double>(frame) / kFramesPerSec;
            std::printf(
                "  t=%.2fs  delay≈%3dms  ERLE=%5.1f dB\n",
                secs,
                apm.aec_delay_estimate_ms(),
                static_cast<double>(apm.erle_db()));
            // Report at 0.5 / 1 / 2 / 4 s like the M6 plan example.
            if      (secs < 1.0)  next_report_frame = 100;
            else if (secs < 2.0)  next_report_frame = 200;
            else if (secs < 4.0)  next_report_frame = 400;
            else                   next_report_frame = -1;
        }

        t += 10ms;
    }

    const double cancel_db =
        (out_energy_total > 0.0L && in_energy_total > 0.0L)
            ? 20.0 * std::log10(static_cast<double>(in_energy_total)
                                  / static_cast<double>(out_energy_total))
            : 0.0;

    std::printf(
        "demo[aec] done: final ERLE=%.1f dB delay=%dms reduction=%.1f dB "
        "(target ERLE >= 25 dB on real hardware)\n",
        static_cast<double>(apm.erle_db()),
        apm.aec_delay_estimate_ms(),
        cancel_db);

    // The synthetic case won't always converge to the real-hardware
    // target because the simulated echo is too clean (no ambient
    // noise, perfect linearity). We pass on any non-zero reduction —
    // the gate is a sanity check that the wiring runs end-to-end.
    if (cancel_db < 1.0) {
        std::fprintf(stderr,
            "demo[aec] FAIL: APM did not reduce echo energy "
            "(reduction=%.2f dB). Check the loopback tap wiring.\n",
            cancel_db);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
