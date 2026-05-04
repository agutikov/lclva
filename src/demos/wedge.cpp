#include "audio/utterance.hpp"
#include "demos/demo.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/openai_tts_client.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acva::demos {

namespace {

constexpr std::string_view kCountingSentence =
    "Конечно! Вот числа от единицы до сотни: "
    "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, "
    "21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, "
    "39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, "
    "57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, "
    "75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, "
    "93, 94, 95, 96, 97, 98, 99, 100.";

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

void print_vram(const char* label) {
    const auto v = snap_vram();
    if (v.first < 0) {
        std::printf("  %-20s vram=n/a (no nvidia-smi)\n", label);
    } else {
        std::printf("  %-20s vram=%ldMiB used / %ldMiB free\n",
                     label, v.first, v.second);
    }
}

} // namespace

// `acva demo wedge` — actively reproduces the Speaches CUDA-OOM
// failure mode and reports the wedged-state diagnosis.
//
// Recipe (matches the production trace from 2026-05-03 161207.log):
//   1. Probe STT health with a 1 s silent transcription. Should
//      succeed; a 500 here means Speaches is ALREADY wedged.
//   2. Issue a TTS request for a 100-number Russian sentence
//      (~430 chars). With cfg.tts.fallback_lang=ru and large-v3 STT
//      already loaded, this typically pushes free VRAM under
//      10 MiB and the request errors with
//      `openai_tts: curl: Transferred a partial file`.
//   3. Probe STT again. If it now returns 500, the Speaches CUDA
//      context is wedged — print the recovery instruction.
//
// The demo PASSES on either outcome (healthy or wedged); the value
// is in the diagnostic report. Useful for triaging stuck-acva
// reports without manually orchestrating the steps.
int run_wedge(const config::Config& cfg, std::span<const std::string> /*args*/) {
    if (cfg.tts.base_url.empty() || cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[wedge] FAIL: cfg.tts not configured\n");
        return EXIT_FAILURE;
    }
    if (cfg.stt.base_url.empty()) {
        std::fprintf(stderr,
            "demo[wedge] FAIL: cfg.stt.base_url empty\n");
        return EXIT_FAILURE;
    }

    std::printf(
        "demo[wedge] reproducer for Speaches CUDA-OOM on long TTS.\n"
        "  tts=%s  voice=%s/%s\n"
        "  stt=%s  model=%s\n\n",
        cfg.tts.base_url.c_str(),
        cfg.tts.voices_resolved.at(cfg.tts.fallback_lang).model_id.c_str(),
        cfg.tts.voices_resolved.at(cfg.tts.fallback_lang).voice_id.c_str(),
        cfg.stt.base_url.c_str(), cfg.stt.model.c_str());

    // ----- 1. Initial STT probe -----
    print_vram("baseline:");
    std::printf("  step 1/3: STT warm-up probe (1 s silent fixture)…\n");
    auto warm = stt::warmup(cfg.stt);
    if (!warm.ok) {
        std::printf(
            "  step 1/3: ✗ STT already wedged (HTTP-500 in %lldms)\n",
            warm.ms);
        std::printf("\ndemo[wedge] verdict: Speaches is ALREADY wedged.\n"
                     "  Recovery: docker restart acva-speaches\n");
        return EXIT_SUCCESS;
    }
    std::printf("  step 1/3: ✓ STT healthy (warm-up %lldms)\n", warm.ms);
    print_vram("after-warmup:");

    // ----- 2. Trigger the long TTS -----
    std::printf("\n  step 2/3: long-TTS request (100-number Russian list, "
                 "%zu chars)…\n", kCountingSentence.size());
    bool tts_err = false;
    std::string tts_err_msg;
    std::uint64_t tts_bytes = 0;
    {
        tts::OpenAiTtsClient client(cfg.tts);
        tts::TtsCallbacks cb;
        cb.on_format = [](int) {};
        cb.on_audio  = [&](std::span<const std::int16_t> s) {
            tts_bytes += s.size() * sizeof(std::int16_t);
        };
        cb.on_finished = [] {};
        cb.on_error  = [&](std::string e) {
            tts_err = true; tts_err_msg = std::move(e);
        };
        const auto t0 = std::chrono::steady_clock::now();
        client.submit(tts::TtsRequest{
            .turn = 1, .seq = 0,
            .text = std::string{kCountingSentence},
            .lang = cfg.tts.fallback_lang,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
        }, cb);
        const auto ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (tts_err) {
            std::printf("  step 2/3: ✗ TTS errored after %lldms, "
                         "%llu bytes received  (err=%s)\n",
                         ms, static_cast<unsigned long long>(tts_bytes),
                         tts_err_msg.c_str());
        } else {
            std::printf("  step 2/3: ✓ TTS completed in %lldms, "
                         "%llu bytes\n",
                         ms, static_cast<unsigned long long>(tts_bytes));
        }
    }
    print_vram("after-tts:");

    // ----- 3. Re-probe STT to detect the wedge -----
    std::printf("\n  step 3/3: STT re-probe…\n");
    auto post = stt::warmup(cfg.stt);
    if (!post.ok) {
        std::printf(
            "  step 3/3: ✗ STT now broken (HTTP-500 in %lldms)\n",
            post.ms);
        print_vram("after-stt:");
        std::printf("\ndemo[wedge] verdict: SUCCESS — wedge reproduced.\n"
                     "  TTS errored mid-stream and the next STT call now\n"
                     "  returns 500 instantly while VRAM stays allocated.\n"
                     "  Recovery: docker restart acva-speaches\n");
        return EXIT_SUCCESS;
    }
    std::printf("  step 3/3: ✓ STT still healthy (warm-up %lldms)\n",
                 post.ms);
    print_vram("after-stt:");
    if (tts_err) {
        std::printf("\ndemo[wedge] verdict: PARTIAL — TTS errored but Speaches\n"
                     "  did NOT wedge. Likely the 100-number request hit a\n"
                     "  Speaches-side timeout rather than CUDA OOM.\n");
    } else {
        std::printf("\ndemo[wedge] verdict: NO-WEDGE — long TTS succeeded.\n"
                     "  Either Speaches has improved, or this hardware has\n"
                     "  enough VRAM headroom (try with llama loaded too).\n");
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
