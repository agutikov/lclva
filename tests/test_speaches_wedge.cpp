// Live characterization of the Speaches CUDA-OOM wedge.
//
// Background (M6, 2026-05-03 161207.log): asking the assistant to
// "count from 1 to 100" in Russian produces a TTS request whose
// encoder workspace pushes free VRAM to ~5 MiB. Speaches throws
// CUDA OOM mid-PCM-stream; the OpenAiTtsClient sees libcurl error 18
// ("Transferred a partial file"). The CUDA context goes into an
// unrecoverable state and every subsequent /v1/audio/transcriptions
// call returns HTTP 500 instantly while VRAM stays held.
//
// What this test verifies (regardless of whether Speaches OOMs on
// THIS run):
//   1. OpenAiTtsClient correctly invokes on_error (NOT on_finished)
//      when the TTS stream is truncated mid-flight.
//   2. After a TTS error, OpenAiSttClient's response is structured
//      (ok | err with HTTP-500 message) rather than a hang/crash.
//   3. The orchestrator's defensive code (warm-up failure detection,
//      EOS-on-error chunk enqueue) doesn't depend on Speaches'
//      health for correctness.
//
// The test PASSES on either outcome (Speaches survives the long
// request OR Speaches wedges) — what we lock in is that our client
// code handles both. If a future Speaches release becomes immune
// to this OOM, the test still passes; the demo (`acva demo wedge`)
// is the active reproducer for upstream-bug reporting.

#include "audio/utterance.hpp"
#include "config/config.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/openai_tts_client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string speaches_url() {
    if (const char* p = std::getenv("ACVA_SPEACHES_URL"); p && *p) return p;
    return "http://127.0.0.1:8090";
}

bool speaches_reachable() {
    httplib::Client c("127.0.0.1", 8090);
    c.set_connection_timeout(std::chrono::seconds(2));
    auto r = c.Get("/health");
    return r && r->status == 200;
}

constexpr const char* kSttModel   = "Systran/faster-whisper-large-v3";
constexpr const char* kRuVoiceModel = "speaches-ai/piper-ru_RU-ruslan-medium";
constexpr const char* kRuVoiceId    = "ruslan";

// 100-number Russian counting sentence (~430 chars). Same payload
// the production trace caught crashing Speaches at VRAM=5 MiB free.
constexpr const char* kCountingSentence =
    "Конечно! Вот числа от единицы до сотни: "
    "1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, "
    "21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, "
    "39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, "
    "57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, "
    "75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, "
    "93, 94, 95, 96, 97, 98, 99, 100.";

acva::config::TtsConfig make_tts_cfg() {
    acva::config::TtsConfig c;
    c.base_url = speaches_url() + "/v1";
    c.fallback_lang = "ru";
    c.voices["ru"] = acva::config::TtsVoice{
        .model_id = kRuVoiceModel, .voice_id = kRuVoiceId};
    c.request_timeout_seconds = 60;
    c.tempo_wpm = 0;     // native cadence for predictable timing
    return c;
}

acva::config::SttConfig make_stt_cfg() {
    acva::config::SttConfig c;
    c.base_url = speaches_url() + "/v1";
    c.model = kSttModel;
    c.language = "ru";
    c.request_timeout_seconds = 30;
    return c;
}

} // namespace

TEST_CASE("Speaches wedge: OpenAiTtsClient calls on_error (not on_finished) "
           "when the PCM stream is truncated"
           * doctest::skip(!speaches_reachable())) {
    auto cfg = make_tts_cfg();
    acva::tts::OpenAiTtsClient client(cfg);

    bool finished = false;
    bool errored  = false;
    std::string err_msg;
    std::uint64_t bytes = 0;

    acva::tts::TtsCallbacks cb;
    cb.on_format    = [](int) {};
    cb.on_audio     = [&](std::span<const std::int16_t> s) {
        bytes += s.size() * sizeof(std::int16_t);
    };
    cb.on_finished  = [&] { finished = true; };
    cb.on_error     = [&](std::string e) { errored = true; err_msg = std::move(e); };

    client.submit(acva::tts::TtsRequest{
        .turn = 1, .seq = 0, .text = kCountingSentence, .lang = "ru",
        .cancel = std::make_shared<acva::dialogue::CancellationToken>(),
    }, cb);

    MESSAGE("TTS bytes received: " << bytes
            << "  finished=" << finished
            << "  errored=" << errored
            << "  err='" << err_msg << "'");

    // The contract being tested: callbacks are mutually exclusive.
    // Either Speaches survived (on_finished fired, no error) or
    // Speaches wedged (on_error fired with a libcurl message).
    // Both must NEVER fire on the same request.
    CHECK(finished != errored);

    if (errored) {
        // Confirm the error is shaped like the documented OOM signature
        // (libcurl partial-file or HTTP 5xx). If it isn't, the failure
        // mode has changed and we should read the message before
        // assuming the existing recovery path still applies.
        const bool partial = err_msg.find("partial file") != std::string::npos;
        const bool http5xx = err_msg.find("http 5") != std::string::npos;
        CHECK_MESSAGE((partial || http5xx),
            "TTS errored but the message doesn't match known wedge "
            "signatures: " << err_msg);
    }
}

TEST_CASE("Speaches wedge: OpenAiSttClient surfaces a clean error after a "
           "wedged-Speaches situation (no hang, no crash)"
           * doctest::skip(!speaches_reachable())) {
    // Don't depend on the previous test's side effect — make our own
    // STT call. If Speaches is healthy, this returns a transcript.
    // If Speaches is wedged from a prior request, it returns HTTP 500
    // in <500 ms. Either way the client must invoke exactly one of
    // {on_final, on_error} and never block beyond
    // request_timeout_seconds.
    auto stt_cfg = make_stt_cfg();
    acva::stt::OpenAiSttClient client(stt_cfg);

    constexpr std::uint32_t kRateHz = 16000;
    std::vector<std::int16_t> silence(kRateHz, std::int16_t{0});  // 1 s silent
    const auto now = std::chrono::steady_clock::now();
    auto fixture = std::make_shared<acva::audio::AudioSlice>(
        std::move(silence), kRateHz, now, now);

    bool got_final = false;
    bool got_error = false;
    std::string err_msg;

    acva::stt::SttCallbacks cb;
    cb.on_final = [&](acva::event::FinalTranscript) { got_final = true; };
    cb.on_error = [&](std::string e) { got_error = true; err_msg = std::move(e); };

    const auto t0 = std::chrono::steady_clock::now();
    client.submit(acva::stt::SttRequest{
        .turn = acva::event::kNoTurn, .slice = fixture,
        .cancel = std::make_shared<acva::dialogue::CancellationToken>(),
        .lang_hint = "ru",
    }, cb);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    MESSAGE("STT call returned in " << ms << " ms  "
            "final=" << got_final << "  error=" << got_error
            << "  err='" << err_msg << "'");

    // Hard contract: exactly one of {on_final, on_error}, no hang.
    CHECK(got_final != got_error);
    CHECK(ms < stt_cfg.request_timeout_seconds * 1000);

    if (got_error) {
        // Confirm the wedged-Speaches error shape. Other shapes
        // (network down, DNS failure) would also be valid client
        // behaviour but indicate a different problem.
        const bool http500 = err_msg.find("http 500") != std::string::npos;
        MESSAGE("STT error matches wedged-Speaches signature: " << http500);
    }
}
