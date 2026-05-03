#include "audio/apm.hpp"
#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/loopback.hpp"
#include "audio/pipeline.hpp"
#include "demos/demo.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

// Stimulus design. Two phases tuned for laptop mics with codec-DSP
// noise suppression (Intel SOF / Realtek ALC firmware aggressively
// removes stationary tones — a pure sine vanishes from the mic feed
// before AEC can see it). Chirp's frequency moves continuously,
// defeating the suppression model; pink noise stresses the full
// spectrum without being predictable.
//
//   • 3 s linear chirp 200 → 2000 Hz
//   • 3 s pink noise
//
// Amplitude bumped to -6 dBFS (was -10) so the speaker emits enough
// to overcome whatever residual suppression remains.
constexpr double  kChirpStartHz  = 200.0;
constexpr double  kChirpEndHz    = 2000.0;
constexpr double  kChirpSec      = 3.0;
constexpr double  kPinkNoiseSec  = 3.0;
constexpr double  kAmplitude     = 0.50;     // -6 dBFS — louder than -10

std::vector<std::int16_t>
build_stimulus(std::uint32_t rate_hz) {
    const auto n_chirp = static_cast<std::size_t>(
        static_cast<double>(rate_hz) * kChirpSec);
    const auto n_pink  = static_cast<std::size_t>(
        static_cast<double>(rate_hz) * kPinkNoiseSec);

    std::vector<std::int16_t> out;
    out.reserve(n_chirp + n_pink);

    // Linear chirp f(t) = f0 + (f1 − f0)·t/T.
    // Instantaneous phase = ∫ 2π·f(t) dt
    //                     = 2π·(f0·t + (f1 − f0)·t²/(2T)).
    for (std::size_t i = 0; i < n_chirp; ++i) {
        const double t  = static_cast<double>(i) / static_cast<double>(rate_hz);
        const double phase = 2.0 * std::numbers::pi_v<double>
            * (kChirpStartHz * t
                + (kChirpEndHz - kChirpStartHz) * t * t / (2.0 * kChirpSec));
        const double s = std::sin(phase);
        out.push_back(static_cast<std::int16_t>(
            std::lround(s * kAmplitude * 32760.0)));
    }
    // Pink noise via Voss-McCartney with 5 octaves. Rougher than a
    // proper -3dB/octave filter but plenty for AEC validation.
    std::mt19937 rng(0xACAFE'F00DU);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    constexpr int kRows = 5;
    double rows[kRows] = {0, 0, 0, 0, 0};
    double running = 0.0;
    for (std::size_t i = 0; i < n_pink; ++i) {
        for (int r = 0; r < kRows; ++r) {
            if ((i & ((1U << r) - 1U)) == 0) {
                running -= rows[r];
                rows[r] = uni(rng);
                running += rows[r];
            }
        }
        // Sum of 5 white sources ≈ ±5; normalize to ±1 then apply amp.
        const double s = running / static_cast<double>(kRows);
        out.push_back(static_cast<std::int16_t>(
            std::lround(s * kAmplitude * 32760.0)));
    }
    return out;
}

} // namespace

// `acva demo aec-hw` — measure real ERLE through the speaker → mic
// loopback path.
//
// Builds the full M6 production pipeline (PlaybackEngine + LoopbackSink
// + CaptureEngine + AudioPipeline + APM), plays a 6-second stimulus
// (3 s of 1 kHz sine + 3 s of pink noise) through the speaker while
// the capture path runs the mic through APM, and reports the
// convergence trajectory of the delay estimator and final ERLE.
//
// Pass criterion (M6 acceptance gate 4): ERLE ≥ 25 dB after
// convergence on a non-headphones setup.
int run_aec_hw(const config::Config& cfg) {
    using namespace std::chrono_literals;

    if (!cfg.audio.capture_enabled) {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: cfg.audio.capture_enabled must be true "
            "(this demo needs the mic open)\n");
        return EXIT_FAILURE;
    }
    if (cfg.audio.output_device == "none") {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: cfg.audio.output_device='none' — this "
            "demo needs a real speaker for the loopback path\n");
        return EXIT_FAILURE;
    }
    if (!cfg.apm.aec_enabled) {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: cfg.apm.aec_enabled is false — nothing "
            "to validate\n");
        return EXIT_FAILURE;
    }

    std::printf(
        "demo[aec-hw] stimulus=chirp_%g-%gHz+pink_noise duration=%.1fs amp=%.2f\n"
        "  output_device='%s' input_device='%s' sample_rate=%uHz\n"
        "  apm: aec=%s ns=%s agc=%s initial_delay=%ums\n\n",
        kChirpStartHz, kChirpEndHz, kChirpSec + kPinkNoiseSec, kAmplitude,
        cfg.audio.output_device.c_str(), cfg.audio.input_device.c_str(),
        cfg.audio.sample_rate_hz,
        cfg.apm.aec_enabled ? "on" : "off",
        cfg.apm.ns_enabled  ? "on" : "off",
        cfg.apm.agc_enabled ? "on" : "off",
        cfg.apm.initial_delay_estimate_ms);

    // ----- 1. Bring up playback + loopback -----
    event::EventBus bus;
    constexpr event::TurnId kFakeTurn = 1;

    playback::PlaybackQueue queue(0);   // unbounded — same as production
    playback::PlaybackEngine engine(cfg.audio, cfg.playback, queue, bus,
                                     []{ return kFakeTurn; });
    const std::size_t loopback_capacity =
        static_cast<std::size_t>(cfg.audio.loopback.ring_seconds)
        * static_cast<std::size_t>(cfg.audio.sample_rate_hz);
    audio::LoopbackSink loopback(loopback_capacity, cfg.audio.sample_rate_hz);
    engine.set_loopback_sink(&loopback);
    if (!engine.start()) {
        std::fprintf(stderr, "demo[aec-hw] playback engine.start() failed\n");
        return EXIT_FAILURE;
    }
    if (engine.headless()) {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: playback ran in headless mode (PortAudio "
            "couldn't open the device). No real audio reached the speaker.\n");
        engine.stop();
        return EXIT_FAILURE;
    }

    // ----- 2. Bring up capture + APM -----
    audio::MonotonicAudioClock clock;
    audio::CaptureRing ring;
    audio::CaptureEngine capture(cfg.audio, ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[aec-hw] capture.start() failed\n");
        engine.stop();
        return EXIT_FAILURE;
    }
    if (capture.headless()) {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: capture ran in headless mode (no mic).\n");
        capture.stop();
        engine.stop();
        return EXIT_FAILURE;
    }

    audio::AudioPipeline::Config apc;
    apc.input_sample_rate           = cfg.audio.sample_rate_hz;
    apc.output_sample_rate          = 16000;
    apc.endpointer.onset_threshold  = cfg.vad.onset_threshold;
    apc.endpointer.offset_threshold = cfg.vad.offset_threshold;
    apc.endpointer.min_speech_ms    = std::chrono::milliseconds{cfg.vad.min_speech_ms};
    apc.endpointer.hangover_ms      = std::chrono::milliseconds{cfg.vad.hangover_ms};
    apc.endpointer.pre_padding_ms   = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.endpointer.post_padding_ms  = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.pre_padding_ms              = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
    apc.post_padding_ms             = std::chrono::milliseconds{cfg.vad.post_padding_ms};
    apc.max_in_flight               = cfg.utterance.max_in_flight;
    apc.max_duration_ms             = std::chrono::milliseconds{cfg.utterance.max_duration_ms};
    apc.vad_model_path              = "";          // VAD off — we don't care about endpointing here
    apc.loopback                    = &loopback;
    apc.apm.aec_enabled             = cfg.apm.aec_enabled;
    apc.apm.ns_enabled              = cfg.apm.ns_enabled;
    apc.apm.agc_enabled             = cfg.apm.agc_enabled;
    apc.apm.initial_delay_estimate_ms = static_cast<int>(cfg.apm.initial_delay_estimate_ms);
    apc.apm.max_delay_ms              = static_cast<int>(cfg.apm.max_delay_ms);

    audio::AudioPipeline pipeline(std::move(apc), ring, clock, bus);
    // Mic-input RMS sampler. Lives in parallel with APM so the
    // report row shows whether the mic is actually picking up the
    // speaker. If mic_rms ~= 0 the speaker→mic acoustic path is
    // broken (headphones plugged in, mic muted, virtual-sink
    // routing) — APM has nothing to cancel and ERLE will be 0.
    //
    // Force the "in_speech" window open via test_probability(1.0)
    // because VAD is off in this demo and the live sink only fires
    // inside the speech window. Endpointer side-effects (a stray
    // SpeechStarted on the bus) are harmless here — nothing
    // subscribes.
    std::atomic<std::uint64_t> mic_rms_sum{0};
    std::atomic<std::uint64_t> mic_rms_count{0};
    pipeline.set_live_audio_sink(
        [&mic_rms_sum, &mic_rms_count](std::span<const std::int16_t> samples) {
            std::uint64_t sumsq = 0;
            for (auto s : samples) {
                const std::int64_t v = s;
                sumsq += static_cast<std::uint64_t>(v * v);
            }
            mic_rms_sum.fetch_add(sumsq, std::memory_order_relaxed);
            mic_rms_count.fetch_add(samples.size(), std::memory_order_relaxed);
        });
    pipeline.set_test_probability(1.0F);
    pipeline.start();

    if (!pipeline.apm() || !pipeline.apm()->aec_active()) {
        std::fprintf(stderr,
            "demo[aec-hw] FAIL: APM not active — built without "
            "webrtc-audio-processing-1?\n");
        pipeline.stop();
        capture.stop();
        engine.stop();
        return EXIT_FAILURE;
    }

    // ----- 3. Stream the stimulus through the playback queue -----
    const auto stimulus = build_stimulus(cfg.audio.sample_rate_hz);
    std::printf("demo[aec-hw] playing stimulus through speakers while capturing…\n\n");
    std::printf("    t        delay_ms    ERLE_dB    frames    mic_rms    loop_frames\n");
    std::printf("  ------    ----------  ---------  --------  ---------  -----------\n");

    const std::size_t buf = cfg.audio.buffer_frames;
    event::SequenceNo seq = 0;
    std::size_t i = 0;

    // Start the convergence printer in parallel with the producer
    // loop so we sample the APM stats every 500 ms regardless of how
    // fast the producer is enqueueing.
    std::atomic<bool> printer_stop{false};
    const auto t_start = std::chrono::steady_clock::now();
    std::thread printer([&]{
        constexpr int kReportPoints[] = {500, 1000, 2000, 4000, 6000};
        std::size_t next = 0;
        while (!printer_stop.load(std::memory_order_acquire)
                && next < std::size(kReportPoints)) {
            const auto target_ms = kReportPoints[next];
            std::this_thread::sleep_until(t_start + std::chrono::milliseconds(target_ms));
            if (printer_stop.load(std::memory_order_acquire)) break;
            const auto* apm = pipeline.apm();
            const auto rms_n = mic_rms_count.load(std::memory_order_relaxed);
            const auto rms_s = mic_rms_sum.load(std::memory_order_relaxed);
            const double rms = rms_n > 0
                ? std::sqrt(static_cast<double>(rms_s) / static_cast<double>(rms_n))
                : 0.0;
            std::printf(
                "  %4.2fs   %8d ms   %6.1f dB   %8llu    %7.0f    %9llu\n",
                target_ms / 1000.0,
                apm->aec_delay_estimate_ms(),
                static_cast<double>(apm->erle_db()),
                static_cast<unsigned long long>(apm->frames_processed()),
                rms,
                static_cast<unsigned long long>(loopback.total_frames_emitted()));
            std::fflush(stdout);
            ++next;
        }
    });

    // Push the stimulus in `buf`-sized chunks at realtime pace. Even
    // though PlaybackQueue is unbounded, pacing keeps the APM seeing
    // a steady reference signal rather than a burst-then-silence.
    auto next_chunk = std::chrono::steady_clock::now();
    const auto chunk_dur = std::chrono::microseconds(
        static_cast<std::int64_t>(buf) * 1'000'000
        / static_cast<std::int64_t>(cfg.audio.sample_rate_hz));
    while (i < stimulus.size()) {
        const std::size_t n = std::min(buf, stimulus.size() - i);
        playback::AudioChunk chunk;
        chunk.turn = kFakeTurn;
        chunk.seq  = seq++;
        const auto first = stimulus.begin() + static_cast<std::ptrdiff_t>(i);
        chunk.samples.assign(first, first + static_cast<std::ptrdiff_t>(n));
        (void)queue.enqueue(std::move(chunk));
        i += n;
        next_chunk += chunk_dur;
        std::this_thread::sleep_until(next_chunk);
    }

    // Final EOS chunk so the engine fires PlaybackFinished.
    {
        playback::AudioChunk eos;
        eos.turn            = kFakeTurn;
        eos.seq             = seq++;
        eos.end_of_sentence = true;
        (void)queue.enqueue(std::move(eos));
    }

    // Wait for playback to drain and for the printer to log the 6 s
    // mark. 1 s of headroom past the last sample.
    const auto deadline = t_start + std::chrono::milliseconds(
        static_cast<long>((kChirpSec + kPinkNoiseSec) * 1000.0) + 1000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    printer_stop.store(true, std::memory_order_release);
    printer.join();

    // ----- 4. Final report + acceptance check -----
    const auto* apm = pipeline.apm();
    const int   delay_ms       = apm->aec_delay_estimate_ms();
    const float erle_db        = apm->erle_db();
    const auto  frames_processed = apm->frames_processed();

    std::printf("\n");
    std::printf(
        "demo[aec-hw] done: final delay=%dms ERLE=%.1f dB "
        "frames_processed=%llu\n",
        delay_ms, static_cast<double>(erle_db),
        static_cast<unsigned long long>(frames_processed));
    std::printf(
        "demo[aec-hw] M6 acceptance gate 4: ERLE >= 25 dB → %s\n",
        (erle_db >= 25.0F) ? "PASS" : "FAIL");

    pipeline.stop();
    capture.stop();
    engine.stop();
    bus.shutdown();

    return (erle_db >= 25.0F) ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace acva::demos
