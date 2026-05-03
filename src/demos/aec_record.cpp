#include "audio/apm.hpp"
#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/loopback.hpp"
#include "audio/resampler.hpp"
#include "audio/wav.hpp"
#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "tts/openai_tts_client.hpp"
#include "tts/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

// Per-channel summary stats — emitted into the stdout report so the
// user can spot bad runs (silent mic, no playback) without opening
// the WAV files.
struct WavStats {
    std::size_t samples = 0;
    std::uint32_t rate_hz = 0;
    double rms = 0.0;
    std::int16_t peak = 0;
    [[nodiscard]] double duration_s() const {
        return rate_hz > 0
            ? static_cast<double>(samples) / static_cast<double>(rate_hz)
            : 0.0;
    }
};

WavStats compute_stats(std::span<const std::int16_t> samples,
                        std::uint32_t rate_hz) {
    WavStats s;
    s.samples = samples.size();
    s.rate_hz = rate_hz;
    if (samples.empty()) return s;
    long double sumsq = 0.0L;
    std::int32_t peak = 0;
    for (auto v : samples) {
        const auto a = std::abs(static_cast<std::int32_t>(v));
        if (a > peak) peak = a;
        sumsq += static_cast<long double>(v) * static_cast<long double>(v);
    }
    s.rms = std::sqrt(static_cast<double>(
                sumsq / static_cast<long double>(samples.size())));
    s.peak = static_cast<std::int16_t>(std::min(peak, 32767));
    return s;
}

std::string env_or(const char* var, std::string_view fallback) {
    if (const char* v = std::getenv(var); v && v[0] != '\0') return std::string(v);
    return std::string(fallback);
}

constexpr std::string_view kDefaultText =
    "The quick brown fox jumps over the lazy dog. "
    "One two three four five. Sssss.";

} // namespace

// `acva demo aec-record` — capture original / raw / cleaned signals at
// every stage of the speaker → air → mic → APM chain into WAV files
// for offline analysis.
//
// M6B Step 1. The whole point of this demo is to give us *signals on
// disk* that any downstream tool (numpy, sox, Audacity) can analyze
// without re-running the C++ binary. The companion `scripts/aec_analyze.py`
// is the canonical analyzer.
//
// Three artifacts in --out-dir (default /tmp/acva-aec-rec/):
//   • original.wav       — TTS output PCM, voice native rate (~22050 Hz)
//   • raw_recording.wav  — mic input, 48 kHz, BEFORE APM
//   • aec_recording.wav  — mic input, 16 kHz, AFTER APM
//
// Note: we deliberately bypass `AudioPipeline` so a bug in
// `apm_carry_` chunking can't masquerade as a hardware issue. Our APM
// here is a standalone instance fed directly from the capture ring.
//
// Env knobs (optional):
//   ACVA_AEC_TEXT         — sentence to synthesize
//   ACVA_AEC_OUT_DIR      — output directory (created if missing)
//   ACVA_AEC_LANG         — tts language; default = cfg.tts.fallback_lang
//   ACVA_AEC_NO_POSTPROC  — when set to "1", force ns_enabled=false +
//                           agc_enabled=false regardless of cfg.apm.*.
//                           AEC stays on. Use this to isolate the AEC
//                           subsystem from AGC, which otherwise lifts
//                           the cleaned signal back up by 5-9 dB and
//                           obscures whether AEC is actually cancelling.
int run_aec_record(const config::Config& cfg) {
    using namespace std::chrono_literals;

    // ----- 0. Validate config + collect knobs -----
    if (cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cfg.tts.voices is empty — populate it before running\n");
        return EXIT_FAILURE;
    }
    if (!cfg.audio.capture_enabled) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cfg.audio.capture_enabled must be true\n");
        return EXIT_FAILURE;
    }
    if (cfg.audio.output_device == "none") {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cfg.audio.output_device='none' — need a real speaker\n");
        return EXIT_FAILURE;
    }

    const std::string text    = env_or("ACVA_AEC_TEXT",    kDefaultText);
    const std::string out_dir = env_or("ACVA_AEC_OUT_DIR", "/tmp/acva-aec-rec");
    const std::string lang    = env_or("ACVA_AEC_LANG",    cfg.tts.fallback_lang);
    const bool no_postproc = env_or("ACVA_AEC_NO_POSTPROC", "") == "1";

    std::error_code mkdir_ec;
    std::filesystem::create_directories(out_dir, mkdir_ec);
    if (mkdir_ec) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: create_directories('%s'): %s\n",
            out_dir.c_str(), mkdir_ec.message().c_str());
        return EXIT_FAILURE;
    }

    const bool apm_aec_eff = cfg.apm.aec_enabled;
    const bool apm_ns_eff  = no_postproc ? false : cfg.apm.ns_enabled;
    const bool apm_agc_eff = no_postproc ? false : cfg.apm.agc_enabled;
    std::printf(
        "demo[aec-record] text=\"%s\" lang=%s out_dir=%s\n"
        "  output_device='%s' input_device='%s' sample_rate=%uHz\n"
        "  apm: aec=%s ns=%s agc=%s initial_delay=%ums%s\n\n",
        text.c_str(), lang.c_str(), out_dir.c_str(),
        cfg.audio.output_device.c_str(), cfg.audio.input_device.c_str(),
        cfg.audio.sample_rate_hz,
        apm_aec_eff ? "on" : "off",
        apm_ns_eff  ? "on" : "off",
        apm_agc_eff ? "on" : "off",
        cfg.apm.initial_delay_estimate_ms,
        no_postproc ? "  [ACVA_AEC_NO_POSTPROC=1: ns/agc forced off]" : "");

    // ----- 1. Synthesize via Speaches; accumulate at native rate -----
    std::printf("demo[aec-record] step 1/4: synthesizing via TTS...\n");
    tts::OpenAiTtsClient tts_client(cfg.tts);
    std::vector<std::int16_t> tts_samples;
    int tts_rate_hz = 0;
    std::string tts_error;

    tts::TtsRequest req{
        .turn = 1,
        .seq  = 0,
        .text = text,
        .lang = lang,
        .cancel = std::make_shared<dialogue::CancellationToken>(),
    };
    tts::TtsCallbacks cb;
    cb.on_format = [&](int rate) { tts_rate_hz = rate; };
    cb.on_audio  = [&](std::span<const std::int16_t> chunk) {
        tts_samples.insert(tts_samples.end(), chunk.begin(), chunk.end());
    };
    cb.on_finished = []{};
    cb.on_error    = [&](std::string e) { tts_error = std::move(e); };

    tts_client.submit(std::move(req), std::move(cb));

    if (!tts_error.empty()) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: TTS error: %s\n", tts_error.c_str());
        return EXIT_FAILURE;
    }
    if (tts_samples.empty() || tts_rate_hz <= 0) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: TTS returned no audio (rate=%d, samples=%zu)\n",
            tts_rate_hz, tts_samples.size());
        return EXIT_FAILURE;
    }

    const std::string original_path = out_dir + "/original.wav";
    if (!audio::write_wav_file(original_path, tts_samples,
                                static_cast<std::uint32_t>(tts_rate_hz))) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cannot write %s\n", original_path.c_str());
        return EXIT_FAILURE;
    }
    const auto original_stats = compute_stats(tts_samples,
                                               static_cast<std::uint32_t>(tts_rate_hz));
    std::printf(
        "  → %s (%.2f s, RMS %.0f, peak %d)\n",
        original_path.c_str(), original_stats.duration_s(),
        original_stats.rms, static_cast<int>(original_stats.peak));

    // ----- 2. Bring up playback + loopback + capture + standalone APM -----
    std::printf("demo[aec-record] step 2/4: bringing up audio loop...\n");

    event::EventBus bus;
    constexpr event::TurnId kFakeTurn = 1;

    playback::PlaybackQueue queue(0);     // unbounded
    playback::PlaybackEngine engine(cfg.audio, cfg.playback, queue, bus,
                                     []{ return kFakeTurn; });
    const std::size_t loopback_capacity =
        static_cast<std::size_t>(cfg.audio.loopback.ring_seconds)
        * static_cast<std::size_t>(cfg.audio.sample_rate_hz);
    audio::LoopbackSink loopback(loopback_capacity, cfg.audio.sample_rate_hz);
    engine.set_loopback_sink(&loopback);

    if (!engine.start()) {
        std::fprintf(stderr, "demo[aec-record] playback engine.start() failed\n");
        return EXIT_FAILURE;
    }
    if (engine.headless()) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: playback ran in headless mode (PortAudio "
            "couldn't open the device). No real audio reached the speaker.\n");
        engine.stop();
        return EXIT_FAILURE;
    }

    audio::MonotonicAudioClock clock;
    audio::CaptureRing capture_ring;
    audio::CaptureEngine capture(cfg.audio, capture_ring, clock);
    if (!capture.start()) {
        std::fprintf(stderr, "demo[aec-record] capture.start() failed\n");
        engine.stop();
        return EXIT_FAILURE;
    }
    if (capture.headless()) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: capture ran in headless mode (no mic).\n");
        capture.stop();
        engine.stop();
        return EXIT_FAILURE;
    }

    audio::ApmConfig apm_cfg;
    apm_cfg.aec_enabled               = cfg.apm.aec_enabled;
    apm_cfg.ns_enabled                = no_postproc ? false : cfg.apm.ns_enabled;
    apm_cfg.agc_enabled               = no_postproc ? false : cfg.apm.agc_enabled;
    apm_cfg.initial_delay_estimate_ms = static_cast<int>(cfg.apm.initial_delay_estimate_ms);
    apm_cfg.max_delay_ms              = static_cast<int>(cfg.apm.max_delay_ms);
    apm_cfg.near_sample_rate_hz       = 16000;
    apm_cfg.reverse_sample_rate_hz    = static_cast<int>(cfg.audio.sample_rate_hz);
    audio::Apm apm(apm_cfg, &loopback);

    // If the user asked for AEC but the wrapper isn't active, the build
    // is missing webrtc-audio-processing-1 and the demo can't measure
    // anything meaningful — bail. If the user intentionally disabled
    // every subsystem (e.g. to record a faithful copy of the mic feed
    // with PipeWire's module-echo-cancel doing the cancellation),
    // pass-through is exactly what we want — proceed.
    if (apm_cfg.aec_enabled && !apm.aec_active()) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: APM AEC requested but not active — "
            "built without webrtc-audio-processing-1?\n");
        capture.stop();
        engine.stop();
        return EXIT_FAILURE;
    }
    if (!apm_cfg.aec_enabled && !apm_cfg.ns_enabled && !apm_cfg.agc_enabled) {
        std::printf(
            "  apm: pass-through mode — aec_recording.wav will be a "
            "faithful resample of raw_recording.wav\n");
    }

    // PlaybackFinished signals end-of-playback (the EOS chunk we
    // enqueue at the end fires this). The drain worker stops 1 s
    // after this fires.
    std::atomic<bool> playback_done{false};
    auto sub_done = bus.subscribe<event::PlaybackFinished>({},
        [&](const event::PlaybackFinished&) { playback_done.store(true); });

    // ----- 3. Drain capture in a worker; raw → 48 k buffer, cleaned → 16 k buffer -----
    std::vector<std::int16_t> raw_48k;
    std::vector<std::int16_t> aec_16k;
    raw_48k.reserve(static_cast<std::size_t>(original_stats.duration_s() + 6.0)
                    * cfg.audio.sample_rate_hz);
    aec_16k.reserve(static_cast<std::size_t>(original_stats.duration_s() + 6.0)
                    * 16000);

    std::atomic<bool> drain_stop{false};
    constexpr std::size_t kApmFrameSamples = 160;       // 10 ms @ 16 kHz
    std::thread drain([&]{
        audio::Resampler rs(static_cast<double>(cfg.audio.sample_rate_hz),
                             16000.0,
                             audio::Resampler::Quality::High);
        std::vector<std::int16_t> apm_carry;
        std::chrono::steady_clock::time_point last_capture_time{};
        while (!drain_stop.load(std::memory_order_acquire)) {
            auto frame_opt = capture_ring.pop();
            if (!frame_opt) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            const auto& frame = *frame_opt;
            const auto view = frame.view();
            raw_48k.insert(raw_48k.end(), view.begin(), view.end());

            const auto down = rs.process(view);
            apm_carry.insert(apm_carry.end(), down.begin(), down.end());
            last_capture_time = frame.captured_at;
            while (apm_carry.size() >= kApmFrameSamples) {
                const auto cleaned = apm.process(
                    std::span<const std::int16_t>{apm_carry.data(),
                                                   kApmFrameSamples},
                    last_capture_time);
                aec_16k.insert(aec_16k.end(), cleaned.begin(), cleaned.end());
                apm_carry.erase(apm_carry.begin(),
                                 apm_carry.begin()
                                 + static_cast<std::ptrdiff_t>(kApmFrameSamples));
            }
        }
    });

    // ----- 4. Resample original → device rate, enqueue at realtime pace -----
    std::printf("demo[aec-record] step 3/4: playing original through speakers...\n");
    audio::Resampler up(static_cast<double>(tts_rate_hz),
                         static_cast<double>(cfg.audio.sample_rate_hz),
                         audio::Resampler::Quality::High);
    auto playback_pcm = up.process(tts_samples);
    {
        auto tail = up.flush();
        playback_pcm.insert(playback_pcm.end(), tail.begin(), tail.end());
    }

    const std::size_t buf = cfg.audio.buffer_frames;
    event::SequenceNo seq = 0;
    std::size_t i = 0;

    auto next_chunk = std::chrono::steady_clock::now();
    const auto chunk_dur = std::chrono::microseconds(
        static_cast<std::int64_t>(buf) * 1'000'000
        / static_cast<std::int64_t>(cfg.audio.sample_rate_hz));
    while (i < playback_pcm.size()) {
        const std::size_t n = std::min(buf, playback_pcm.size() - i);
        playback::AudioChunk chunk;
        chunk.turn = kFakeTurn;
        chunk.seq  = seq++;
        const auto first = playback_pcm.begin() + static_cast<std::ptrdiff_t>(i);
        chunk.samples.assign(first, first + static_cast<std::ptrdiff_t>(n));
        (void)queue.enqueue(std::move(chunk));
        i += n;
        next_chunk += chunk_dur;
        std::this_thread::sleep_until(next_chunk);
    }

    // EOS sentinel — fires PlaybackFinished once the audio thread drains it.
    {
        playback::AudioChunk eos;
        eos.turn            = kFakeTurn;
        eos.seq             = seq++;
        eos.end_of_sentence = true;
        (void)queue.enqueue(std::move(eos));
    }

    // ----- 5. Wait for drain — 1 s after PlaybackFinished, hard cap -----
    const auto playback_dur =
        std::chrono::milliseconds(static_cast<std::int64_t>(
            original_stats.duration_s() * 1000.0));
    const auto hard_deadline = std::chrono::steady_clock::now()
                                  + playback_dur + 5s;
    while (std::chrono::steady_clock::now() < hard_deadline
           && !playback_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(20ms);
    }
    // Always give 1 s of room tail past PlaybackFinished (or until the
    // hard deadline if the event never fired).
    const auto tail_deadline = std::min(
        hard_deadline,
        std::chrono::steady_clock::now() + 1s);
    while (std::chrono::steady_clock::now() < tail_deadline) {
        std::this_thread::sleep_for(20ms);
    }

    drain_stop.store(true, std::memory_order_release);
    drain.join();

    // ----- 6. Tear down + write WAVs + report -----
    std::printf("demo[aec-record] step 4/4: writing WAV files...\n");

    sub_done->stop();
    capture.stop();
    engine.stop();
    bus.shutdown();

    const std::string raw_path = out_dir + "/raw_recording.wav";
    const std::string aec_path = out_dir + "/aec_recording.wav";

    if (!audio::write_wav_file(raw_path, raw_48k, cfg.audio.sample_rate_hz)) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cannot write %s\n", raw_path.c_str());
        return EXIT_FAILURE;
    }
    if (!audio::write_wav_file(aec_path, aec_16k, 16000)) {
        std::fprintf(stderr,
            "demo[aec-record] FAIL: cannot write %s\n", aec_path.c_str());
        return EXIT_FAILURE;
    }

    const auto raw_stats = compute_stats(raw_48k, cfg.audio.sample_rate_hz);
    const auto aec_stats = compute_stats(aec_16k, 16000);

    std::printf("\ndemo[aec-record] done.\n\n");
    std::printf("  %-22s  %7s  %8s  %8s  %5s\n",
                 "file", "rate", "duration", "RMS", "peak");
    std::printf("  %-22s  %5uHz  %7.2fs  %8.0f  %5d\n",
                 "original.wav",
                 original_stats.rate_hz, original_stats.duration_s(),
                 original_stats.rms, static_cast<int>(original_stats.peak));
    std::printf("  %-22s  %5uHz  %7.2fs  %8.0f  %5d\n",
                 "raw_recording.wav",
                 raw_stats.rate_hz, raw_stats.duration_s(),
                 raw_stats.rms, static_cast<int>(raw_stats.peak));
    std::printf("  %-22s  %5uHz  %7.2fs  %8.0f  %5d\n",
                 "aec_recording.wav",
                 aec_stats.rate_hz, aec_stats.duration_s(),
                 aec_stats.rms, static_cast<int>(aec_stats.peak));

    std::printf(
        "\nAPM self-reported: delay=%dms ERLE=%.1f dB frames=%llu\n",
        apm.aec_delay_estimate_ms(),
        static_cast<double>(apm.erle_db()),
        static_cast<unsigned long long>(apm.frames_processed()));

    std::printf(
        "\nNext: scripts/aec_analyze.py %s\n"
        "  → cross-correlation delay, per-band attenuation, AEC residual\n",
        out_dir.c_str());

    if (raw_stats.rms < 50.0) {
        std::fprintf(stderr,
            "\ndemo[aec-record] WARN: raw_recording RMS is very low "
            "(%.0f). Mic muted? Speaker silent? Headphones plugged in?\n",
            raw_stats.rms);
    }

    return EXIT_SUCCESS;
}

} // namespace acva::demos
