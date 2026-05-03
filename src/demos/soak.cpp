#include "audio/utterance.hpp"
#include "demos/demo.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/openai_tts_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

// 5 minutes of sustained TTS + STT load. Long enough to surface a
// slow VRAM leak (a leak of even 1 MiB / request shows up clearly
// against the per-second VRAM print).
constexpr int kDurationSec = 300;

// Crude nearest-neighbour 22050 → 16000 Hz resample. Same code path
// as `acva demo stt` — fine for a fixture sentence used as STT input.
std::vector<std::int16_t>
downsample_22050_to_16000(std::vector<std::int16_t>&& src) {
    std::vector<std::int16_t> out;
    const double ratio = 16000.0 / 22050.0;
    const std::size_t n = static_cast<std::size_t>(
        static_cast<double>(src.size()) * ratio);
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t s = static_cast<std::size_t>(
            static_cast<double>(i) / ratio);
        out.push_back(s < src.size() ? src[s] : 0);
    }
    return out;
}

// Shell out to nvidia-smi for `used,free` MiB. Returns (-1, -1) when
// the binary isn't on PATH or the parse fails. Cheap enough at 1 Hz.
std::pair<long, long> snap_vram() {
    FILE* p = popen(
        "nvidia-smi --query-gpu=memory.used,memory.free "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) return {-1, -1};
    long used = -1, free_ = -1;
    if (std::fscanf(p, " %ld , %ld", &used, &free_) != 2) {
        used = -1; free_ = -1;
    }
    pclose(p);
    return {used, free_};
}

struct Counters {
    std::atomic<std::uint64_t> req{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<std::uint64_t> total_ms{0};      // sum of per-request wall-time
    std::atomic<std::uint64_t> total_bytes{0};
    std::atomic<std::uint64_t> total_audio_ms{0}; // sum of audio durations

    // Per-window aggregates for the per-second RTF print. Main thread
    // snapshots-and-resets each tick; per-request stats stay in
    // `samples` for the final summary.
    std::atomic<std::uint64_t> w_count{0};
    std::atomic<std::uint64_t> w_sum_ms{0};
    std::atomic<std::uint64_t> w_audio_ms{0};

    // Per-request samples, recorded for the end-of-run percentile
    // table. Mutex over a vector — workers are 1-2 threads writing at
    // request cadence (tens of Hz max), so contention is trivial.
    std::mutex                 samples_mu;
    std::vector<std::uint64_t> samples;
};

void record(Counters& c, std::uint64_t ms,
             std::uint64_t audio_ms, std::uint64_t bytes) {
    c.req.fetch_add(1, std::memory_order_relaxed);
    c.total_ms.fetch_add(ms, std::memory_order_relaxed);
    c.total_audio_ms.fetch_add(audio_ms, std::memory_order_relaxed);
    c.total_bytes.fetch_add(bytes, std::memory_order_relaxed);
    c.w_count.fetch_add(1, std::memory_order_relaxed);
    c.w_sum_ms.fetch_add(ms, std::memory_order_relaxed);
    c.w_audio_ms.fetch_add(audio_ms, std::memory_order_relaxed);
    {
        std::lock_guard lk(c.samples_mu);
        c.samples.push_back(ms);
    }
}

void tts_worker(const config::Config& cfg, Counters& c,
                 std::atomic<bool>& stop) {
    constexpr std::string_view kSentence =
        "Hello from acva. The moon is bright tonight. "
        "This is a soak-test sentence used to keep the TTS pipeline "
        "saturated for many minutes.";

    tts::OpenAiTtsClient client(cfg.tts);
    constexpr int kTtsRateHz = 22050;   // Speaches Piper output rate
    while (!stop.load(std::memory_order_acquire)) {
        std::uint64_t samples = 0;
        std::uint64_t bytes = 0;
        bool err = false;
        tts::TtsCallbacks cb;
        cb.on_format = [](int) {};
        cb.on_audio  = [&](std::span<const std::int16_t> s) {
            samples += s.size();
            bytes += s.size() * sizeof(std::int16_t);
        };
        cb.on_finished = [] {};
        cb.on_error  = [&](std::string) { err = true; };

        const auto t0 = std::chrono::steady_clock::now();
        client.submit(tts::TtsRequest{
            .turn = 1, .seq = 0, .text = std::string{kSentence},
            .lang = cfg.tts.fallback_lang,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
        }, cb);
        const auto ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (err) {
            c.errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            const std::uint64_t audio_ms = samples * 1000U / kTtsRateHz;
            record(c, ms, audio_ms, bytes);
        }
    }
}

void stt_worker(const config::Config& cfg,
                 std::shared_ptr<audio::AudioSlice> fixture,
                 Counters& c, std::atomic<bool>& stop) {
    stt::OpenAiSttClient client(cfg.stt);
    const std::uint64_t fixture_audio_ms = fixture->sample_rate() > 0
        ? static_cast<std::uint64_t>(fixture->samples().size()) * 1000U
              / fixture->sample_rate()
        : 0U;
    while (!stop.load(std::memory_order_acquire)) {
        bool err = false;
        stt::SttCallbacks cb;
        cb.on_final = [&](event::FinalTranscript) {};
        cb.on_error = [&](std::string) { err = true; };

        const auto t0 = std::chrono::steady_clock::now();
        client.submit(stt::SttRequest{
            .turn = 1, .slice = fixture,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
            .lang_hint = cfg.stt.language,
        }, cb);
        const auto ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (err) {
            c.errors.fetch_add(1, std::memory_order_relaxed);
        } else {
            record(c, ms, fixture_audio_ms, /*bytes=*/0);
        }
    }
}

} // namespace

int run_soak(const config::Config& cfg) {
    using namespace std::chrono_literals;

    if (cfg.tts.base_url.empty() || cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[soak] FAIL: cfg.tts not configured\n");
        return EXIT_FAILURE;
    }
    if (cfg.stt.base_url.empty()) {
        std::fprintf(stderr,
            "demo[soak] FAIL: cfg.stt.base_url empty\n");
        return EXIT_FAILURE;
    }

    std::printf(
        "demo[soak] target=Speaches duration=%ds\n"
        "demo[soak] tts=%s voice=%s/%s\n"
        "demo[soak] stt=%s model=%s\n",
        kDurationSec,
        cfg.tts.base_url.c_str(),
        cfg.tts.voices.at(cfg.tts.fallback_lang).model_id.c_str(),
        cfg.tts.voices.at(cfg.tts.fallback_lang).voice_id.c_str(),
        cfg.stt.base_url.c_str(), cfg.stt.model.c_str());

    // ----- 1. Synthesize the STT fixture once. -----
    std::printf("demo[soak] generating STT fixture (one-shot TTS)…\n");
    std::vector<std::int16_t> fixture_22k;
    bool synth_err = false;
    {
        tts::OpenAiTtsClient warmup_tts(cfg.tts);
        tts::TtsCallbacks cb;
        cb.on_format = [](int) {};
        cb.on_audio  = [&](std::span<const std::int16_t> s) {
            fixture_22k.insert(fixture_22k.end(), s.begin(), s.end());
        };
        cb.on_finished = [] {};
        cb.on_error  = [&](std::string e) {
            synth_err = true;
            std::fprintf(stderr, "demo[soak] fixture synth: %s\n", e.c_str());
        };
        warmup_tts.submit(tts::TtsRequest{
            .turn = 1, .seq = 0,
            .text = "Hello from acva. The moon is bright tonight.",
            .lang = cfg.tts.fallback_lang,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
        }, cb);
    }
    if (synth_err || fixture_22k.empty()) {
        std::fprintf(stderr, "demo[soak] FAIL: could not generate fixture\n");
        return EXIT_FAILURE;
    }
    auto fixture_16k = downsample_22050_to_16000(std::move(fixture_22k));
    const double fixture_sec =
        static_cast<double>(fixture_16k.size()) / 16000.0;
    auto fixture = std::make_shared<audio::AudioSlice>(
        std::move(fixture_16k), 16000U,
        std::chrono::steady_clock::now(),
        std::chrono::steady_clock::now());
    std::printf("demo[soak] fixture: %.2fs of audio @ 16 kHz\n", fixture_sec);

    // ----- 2. Force Whisper model load BEFORE measurement starts so
    //          the first per-second tick already includes the model in
    //          VRAM. Without this, the soak's t=1..N seconds capture
    //          the cold-load transient instead of steady-state.
    std::printf("demo[soak] warming up STT model (blocking)…\n");
    const auto warmup = stt::warmup(cfg.stt);
    if (warmup.ok) {
        std::printf("demo[soak] STT warm-up: %lldms\n", warmup.ms);
    } else {
        std::fprintf(stderr,
            "demo[soak] STT warm-up failed in %lldms (%s); "
            "first STT request will pay the model-load cost\n",
            warmup.ms, warmup.error.c_str());
    }

    // ----- 3. Spawn workers + per-second monitor. -----
    Counters tts_c, stt_c;
    std::atomic<bool> stop{false};

    auto vram_baseline = snap_vram();
    long vram_max_used  = vram_baseline.first;
    long vram_min_free  = vram_baseline.second;
    int  vram_max_at_s  = 0;
    std::printf("demo[soak] baseline VRAM: used=%ldMiB free=%ldMiB\n",
                 vram_baseline.first, vram_baseline.second);

    std::thread tts_t(tts_worker, std::cref(cfg), std::ref(tts_c), std::ref(stop));
    std::thread stt_t(stt_worker, std::cref(cfg), fixture,
                       std::ref(stt_c), std::ref(stop));

    std::printf("\n");
    std::printf(
        "   t      TTS r/s  RTF    STT r/s  RTF    err  vram used/free  peak\n");
    std::printf(
        "  ----   --------  -----  --------  -----  ---  --------------  ------\n");

    auto snap_window = [](Counters& c)
        -> std::tuple<std::uint64_t, std::uint64_t, std::uint64_t> {
        const auto count    = c.w_count.exchange(0, std::memory_order_relaxed);
        const auto sum_ms   = c.w_sum_ms.exchange(0, std::memory_order_relaxed);
        const auto audio_ms = c.w_audio_ms.exchange(0, std::memory_order_relaxed);
        return {count, sum_ms, audio_ms};
    };

    auto fmt_rtf = [](std::uint64_t audio_ms, std::uint64_t wall_ms,
                       char* dest, std::size_t n) {
        if (wall_ms == 0) {
            std::snprintf(dest, n, "  -  ");
            return;
        }
        const double rtf = static_cast<double>(audio_ms)
                              / static_cast<double>(wall_ms);
        std::snprintf(dest, n, "x%4.2f", rtf);
    };

    const auto t_start = std::chrono::steady_clock::now();
    for (int sec = 1; sec <= kDurationSec; ++sec) {
        std::this_thread::sleep_for(1s);

        auto [tts_n, tts_sum, tts_aud] = snap_window(tts_c);
        auto [stt_n, stt_sum, stt_aud] = snap_window(stt_c);
        const auto errors = tts_c.errors.load(std::memory_order_relaxed)
                              + stt_c.errors.load(std::memory_order_relaxed);

        char tts_rtf[8], stt_rtf[8];
        fmt_rtf(tts_aud, tts_sum, tts_rtf, sizeof(tts_rtf));
        fmt_rtf(stt_aud, stt_sum, stt_rtf, sizeof(stt_rtf));

        const auto vram = snap_vram();
        if (vram.first > vram_max_used) {
            vram_max_used = vram.first;
            vram_max_at_s = sec;
        }
        if (vram.second >= 0
            && (vram_min_free < 0 || vram.second < vram_min_free)) {
            vram_min_free = vram.second;
        }
        std::printf(
            "  %4ds   %4llu r/s  %s  %4llu r/s  %s  %3llu  %5ldM/%5ldM  %5ldM\n",
            sec,
            static_cast<unsigned long long>(tts_n), tts_rtf,
            static_cast<unsigned long long>(stt_n), stt_rtf,
            static_cast<unsigned long long>(errors),
            vram.first, vram.second, vram_max_used);
        std::fflush(stdout);
    }

    stop.store(true, std::memory_order_release);
    tts_t.join();
    stt_t.join();

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - t_start).count();
    const auto vram_final = snap_vram();
    const auto tts_req = tts_c.req.load(std::memory_order_relaxed);
    const auto stt_req = stt_c.req.load(std::memory_order_relaxed);
    const auto tts_err = tts_c.errors.load(std::memory_order_relaxed);
    const auto stt_err = stt_c.errors.load(std::memory_order_relaxed);
    const auto tts_b   = tts_c.total_bytes.load(std::memory_order_relaxed);

    std::printf("\n");
    std::printf("demo[soak] done after %llds\n",
                 static_cast<long long>(elapsed));
    const double elapsed_d = static_cast<double>(elapsed);

    auto rtf = [](const Counters& c) {
        const auto wall  = c.total_ms.load(std::memory_order_relaxed);
        const auto audio = c.total_audio_ms.load(std::memory_order_relaxed);
        return wall == 0 ? 0.0 : static_cast<double>(audio)
                                    / static_cast<double>(wall);
    };

    auto pct = [](const std::vector<std::uint64_t>& sorted, double p) {
        if (sorted.empty()) return 0ULL;
        const std::size_t i = std::min(
            sorted.size() - 1,
            static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
        return static_cast<unsigned long long>(sorted[i]);
    };

    auto print_stats = [&](const char* name, Counters& c, double rtf_v,
                            std::uint64_t req, std::uint64_t err,
                            std::uint64_t bytes) {
        std::vector<std::uint64_t> samples;
        {
            std::lock_guard lk(c.samples_mu);
            samples = c.samples;
        }
        std::sort(samples.begin(), samples.end());
        const std::uint64_t mean_ms = samples.empty() ? 0ULL :
            std::accumulate(samples.begin(), samples.end(), std::uint64_t{0})
            / samples.size();

        std::printf("demo[soak] %s\n", name);
        std::printf("           req=%llu  err=%llu  bytes=%llu  avg=%.1fr/s  RTF=x%.2f\n",
                     static_cast<unsigned long long>(req),
                     static_cast<unsigned long long>(err),
                     static_cast<unsigned long long>(bytes),
                     elapsed > 0 ? static_cast<double>(req) / elapsed_d : 0.0,
                     rtf_v);
        if (samples.empty()) {
            std::printf("           latency: <no samples>\n");
            return;
        }
        std::printf("           latency ms:  min=%llu  mean=%llu  "
                     "p50=%llu  p90=%llu  p95=%llu  p99=%llu  max=%llu\n",
                     static_cast<unsigned long long>(samples.front()),
                     static_cast<unsigned long long>(mean_ms),
                     pct(samples, 0.50), pct(samples, 0.90),
                     pct(samples, 0.95), pct(samples, 0.99),
                     static_cast<unsigned long long>(samples.back()));
    };

    print_stats("TTS", tts_c, rtf(tts_c), tts_req, tts_err, tts_b);
    print_stats("STT", stt_c, rtf(stt_c), stt_req, stt_err, /*bytes=*/0);
    std::printf("demo[soak] VRAM: baseline used=%ldMiB → final used=%ldMiB "
                 "(delta %+ldMiB)\n",
                 vram_baseline.first, vram_final.first,
                 vram_final.first - vram_baseline.first);
    std::printf("demo[soak] VRAM peak: used=%ldMiB at t=%ds  "
                 "(min free=%ldMiB)\n",
                 vram_max_used, vram_max_at_s, vram_min_free);
    if (tts_err + stt_err > 0) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

} // namespace acva::demos
