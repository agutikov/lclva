#include "demos/demo.hpp"

#include "audio/utterance.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/openai_tts_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

// Crude nearest-neighbour 22050 → 16000 Hz resample. The TTS demo
// fixture is a short sentence; this loses high-frequency content but
// keeps Whisper's accuracy more than good enough for a smoke check.
std::vector<std::int16_t> downsample_22050_to_16000(
    std::vector<std::int16_t>&& src) {
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

} // namespace

int run_stt(const config::Config& cfg) {
    if (cfg.stt.base_url.empty()) {
        std::fprintf(stderr,
            "demo[stt] FAIL: cfg.stt.base_url is empty — Speaches not configured\n");
        return EXIT_FAILURE;
    }
    if (cfg.tts.base_url.empty() || cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[stt] FAIL: cfg.tts is not configured — the demo synthesizes "
            "its fixture audio via TTS first\n");
        return EXIT_FAILURE;
    }

    constexpr std::string_view kSentence =
        "Hello from acva. The moon is bright tonight.";
    constexpr std::string_view kWarmup = "Hi.";

    std::printf("demo[stt] tts=%s stt=%s model=%s\n",
                 cfg.tts.base_url.c_str(),
                 cfg.stt.base_url.c_str(),
                 cfg.stt.model.c_str());

    tts::OpenAiTtsClient tts(cfg.tts);
    stt::OpenAiSttClient stt(cfg.stt);

    // Local helpers — synth a string into a 16 kHz AudioSlice, and
    // transcribe a slice. Both report wall-clock time.
    auto synth = [&](std::string_view text)
                       -> std::pair<std::shared_ptr<audio::AudioSlice>, long long> {
        std::vector<std::int16_t> buf;
        bool err = false; std::string err_msg;
        tts::TtsCallbacks tcb;
        tcb.on_format = [](int) {};
        tcb.on_audio  = [&](std::span<const std::int16_t> s) {
            buf.insert(buf.end(), s.begin(), s.end());
        };
        tcb.on_finished = [] {};
        tcb.on_error  = [&](std::string e) { err = true; err_msg = std::move(e); };

        const auto t0 = std::chrono::steady_clock::now();
        tts.submit(tts::TtsRequest{
            .turn = 1, .seq = 0, .text = std::string{text},
            .lang = cfg.tts.fallback_lang,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
        }, tcb);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        if (err) {
            std::fprintf(stderr, "demo[stt] FAIL: tts: %s\n", err_msg.c_str());
            return {nullptr, ms};
        }
        auto a16 = downsample_22050_to_16000(std::move(buf));
        auto slice = std::make_shared<audio::AudioSlice>(
            std::move(a16), 16000,
            std::chrono::steady_clock::now(),
            std::chrono::steady_clock::now());
        return {slice, ms};
    };

    // Returns (transcript, ms, ok). `ok=false` means stt errored.
    auto transcribe = [&](std::shared_ptr<audio::AudioSlice> slice)
                          -> std::tuple<std::string, long long, bool> {
        std::string text, err;
        stt::SttCallbacks scb;
        scb.on_final = [&](event::FinalTranscript ft) {
            text = std::move(ft.text);
        };
        scb.on_error = [&](std::string e) { err = std::move(e); };

        const auto t0 = std::chrono::steady_clock::now();
        stt.submit(stt::SttRequest{
            .turn = 1, .slice = slice,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
            .lang_hint = cfg.stt.language,
        }, scb);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (!err.empty()) {
            std::fprintf(stderr, "demo[stt] FAIL: stt: %s\n", err.c_str());
            return {"", ms, false};
        }
        return {std::move(text), ms, true};
    };

    // Warmup pass — load Piper voice + Whisper model into VRAM. Only
    // the first call to each endpoint pays the model-load cost (with
    // Speaches' default 300 s eviction we'll stay warm for the
    // measured pass below).
    std::printf("demo[stt] warming up models…\n");
    auto [warm_slice, warm_tts_ms] = synth(kWarmup);
    if (!warm_slice) return EXIT_FAILURE;
    auto [warm_text, warm_stt_ms, warm_ok] = transcribe(warm_slice);
    if (!warm_ok) return EXIT_FAILURE;
    std::printf("demo[stt]   warmup tts=%lldms stt=%lldms (cold-load + transfer)\n",
                 warm_tts_ms, warm_stt_ms);

    // Measured pass — both models are warm, this is a clean
    // round-trip number.
    std::printf("demo[stt] measured pass…\n");
    auto [slice, tts_ms] = synth(kSentence);
    if (!slice) return EXIT_FAILURE;
    const auto fixture_dur =
        slice->sample_rate() > 0
          ? static_cast<double>(slice->samples().size())
                / static_cast<double>(slice->sample_rate())
          : 0.0;
    std::printf("demo[stt]   tts=%lldms (fixture=%.2fs)\n",
                 tts_ms, fixture_dur);

    auto [transcript, stt_ms, ok] = transcribe(slice);
    if (!ok) return EXIT_FAILURE;
    std::printf("demo[stt]   stt=%lldms transcript=\"%s\"\n",
                 stt_ms, transcript.c_str());

    const auto rtf = fixture_dur > 0.0
        ? static_cast<double>(stt_ms) / 1000.0 / fixture_dur
        : 0.0;
    std::printf("demo[stt] warm round-trip: tts=%lldms + stt=%lldms = %lldms"
                 " (stt RTF %.2fx)\n",
                 tts_ms, stt_ms, tts_ms + stt_ms, rtf);
    if (transcript.find("Hello") == std::string::npos) {
        std::fprintf(stderr,
            "demo[stt] WARN: transcript missing the expected 'Hello' prefix — "
            "model may have heard something else\n");
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
