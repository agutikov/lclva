// Live integration smoke against the Speaches container.
//
// Runs as part of the integration suite (`acva_integration_tests`).
// Cases probe the live container at ${ACVA_SPEACHES_URL:-http://127.0.0.1:8090}
// and skip cleanly when it isn't reachable, so a teammate without the
// stack up still gets a passing run. The dev workstation has the
// stack always running, so nothing skips there.
//
// What's verified:
//   1. /health 200
//   2. POST /v1/audio/speech with a Piper voice — valid PCM stream
//      (functional only; cpp-httplib's Post(body, ...) overloads
//      don't support ContentReceiver, so streaming TTFB is verified
//      by the production OpenAiTtsClient via libcurl in M4B Step 4)
//   3. POST /v1/audio/transcriptions on the TTS output round-trips

#include "audio/utterance.hpp"
#include "config/config.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/openai_tts_client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string speaches_url() {
    if (const char* p = std::getenv("ACVA_SPEACHES_URL"); p && *p) {
        return p;
    }
    return "http://127.0.0.1:8090";
}

struct ParsedUrl {
    std::string host;
    int         port = 0;
    bool        ok   = false;
};

ParsedUrl parse(const std::string& url) {
    ParsedUrl r;
    static const std::string kPrefix = "http://";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) return r;
    auto rest      = url.substr(kPrefix.size());
    auto slash     = rest.find('/');
    auto authority = rest.substr(0, slash == std::string::npos ? rest.size() : slash);
    auto colon     = authority.find(':');
    r.host = authority.substr(0, colon);
    r.port = colon == std::string::npos ? 80 : std::atoi(authority.substr(colon + 1).c_str());
    r.ok   = !r.host.empty() && r.port > 0;
    return r;
}

bool speaches_reachable() {
    auto p = parse(speaches_url());
    if (!p.ok) return false;
    httplib::Client c(p.host, p.port);
    c.set_connection_timeout(std::chrono::seconds(2));
    auto r = c.Get("/health");
    return r && r->status == 200;
}

httplib::Client make_client() {
    auto p = parse(speaches_url());
    REQUIRE(p.ok);
    httplib::Client c(p.host, p.port);
    c.set_connection_timeout(std::chrono::seconds(5));
    c.set_read_timeout(std::chrono::seconds(60));
    return c;
}

constexpr const char* kVoiceModel = "speaches-ai/piper-en_US-amy-medium";
// Production STT model — same as `config/default.yaml` and
// `scripts/download-stt.sh`. Tests must match the production model
// so a flake here means a real production problem, not a smoke/prod
// mismatch. Switched from large-v3-turbo to faster-whisper-medium
// on 2026-05-03 after the M6 OOM episode (turbo-ct2 + llama-7B-Q4
// left ~165 MiB free on the 8 GB RTX 4060, blowing through the
// encoder workspace under sustained load).
constexpr const char* kSttModel   = "Systran/faster-whisper-medium";
constexpr const char* kSentence   = "Hello from acva. This is a smoke test.";

} // namespace

TEST_CASE("Speaches smoke: /health"
           * doctest::skip(!speaches_reachable())) {
    auto c   = make_client();
    auto res = c.Get("/health");
    REQUIRE(res);
    CHECK(res->status == 200);
}

TEST_CASE("Speaches smoke: TTS POST /v1/audio/speech returns PCM"
           * doctest::skip(!speaches_reachable())) {
    auto c = make_client();

    std::string body = std::string(R"({"model":")") + kVoiceModel
                       + R"(","input":")" + kSentence
                       + R"(","voice":"amy","response_format":"pcm"})";

    auto res = c.Post("/v1/audio/speech", body, "application/json");
    REQUIRE(res);
    CHECK(res->status == 200);
    // PCM is bare int16 mono 22050 Hz; ~3 s of speech ⇒ at least 50 KB.
    CHECK(res->body.size() >= 50'000);
    // Bytes should be even (int16 frames).
    CHECK(res->body.size() % 2 == 0);
}

TEST_CASE("Speaches smoke: STT round-trip on the TTS output"
           * doctest::skip(!speaches_reachable())) {
    auto c = make_client();

    std::string wav;
    {
        std::string body = std::string(R"({"model":")") + kVoiceModel
                           + R"(","input":")" + kSentence
                           + R"(","voice":"amy","response_format":"wav"})";
        auto res = c.Post("/v1/audio/speech", body, "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        wav = std::move(res->body);
        REQUIRE(wav.size() > 1024);
    }

    httplib::MultipartFormDataItems items{
        {"file",  wav,      "speech.wav", "audio/wav"},
        {"model", kSttModel, "",           ""},
    };
    auto res = c.Post("/v1/audio/transcriptions", items);
    REQUIRE(res);
    CHECK(res->status == 200);
    INFO("transcription body=" << res->body);
    // Tiny model may misspell "acva"; assert on a robust prefix.
    CHECK(res->body.find("Hello") != std::string::npos);
}

// Decode a 22050 Hz 16-bit-mono WAV body into int16 samples and
// resample (nearest-neighbour, fine for a smoke) to 16 kHz so the
// OpenAiSttClient can take it as an AudioSlice.
namespace {
std::vector<std::int16_t> wav_to_16k(const std::string& wav) {
    if (wav.size() <= 44) return {};
    std::vector<std::int16_t> samples_22k(
        (wav.size() - 44) / 2);
    std::memcpy(samples_22k.data(), wav.data() + 44, samples_22k.size() * 2);
    // Nearest-neighbour 22050 → 16000; fine for smoke (audio is short
    // and the resulting transcription only needs to contain "Hello").
    std::vector<std::int16_t> samples_16k;
    samples_16k.reserve(samples_22k.size() * 16000 / 22050);
    const double ratio = 16000.0 / 22050.0;
    const std::size_t n = static_cast<std::size_t>(
        static_cast<double>(samples_22k.size()) * ratio);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t src = static_cast<std::size_t>(
            static_cast<double>(i) / ratio);
        samples_16k.push_back(src < samples_22k.size() ? samples_22k[src] : 0);
    }
    return samples_16k;
}
} // namespace

TEST_CASE("Speaches smoke: OpenAiSttClient end-to-end"
           * doctest::skip(!speaches_reachable())) {
    // Synthesize via Speaches first so we have known audio.
    auto http = make_client();
    std::string wav;
    {
        std::string body = std::string(R"({"model":")") + kVoiceModel
                           + R"(","input":")" + kSentence
                           + R"(","voice":"amy","response_format":"wav"})";
        auto res = http.Post("/v1/audio/speech", body, "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        wav = std::move(res->body);
    }
    auto samples_16k = wav_to_16k(wav);
    REQUIRE(!samples_16k.empty());

    // Wrap in an AudioSlice and feed through the production STT
    // client.
    auto slice = std::make_shared<acva::audio::AudioSlice>(
        std::move(samples_16k), 16000,
        std::chrono::steady_clock::now(), std::chrono::steady_clock::now());

    acva::config::SttConfig cfg;
    cfg.base_url = speaches_url() + "/v1";
    cfg.model    = kSttModel;
    cfg.request_timeout_seconds = 30;

    acva::stt::OpenAiSttClient stt(cfg);

    std::string transcript;
    std::string error;
    acva::stt::SttCallbacks cb;
    cb.on_final = [&](acva::event::FinalTranscript ft) {
        transcript = std::move(ft.text);
    };
    cb.on_error = [&](std::string e) { error = std::move(e); };

    stt.submit(acva::stt::SttRequest{
        .turn      = 1,
        .slice     = slice,
        .cancel    = std::make_shared<acva::dialogue::CancellationToken>(),
        .lang_hint = "",
    }, cb);

    INFO("transcript=" << transcript << " error=" << error);
    CHECK(error.empty());
    CHECK(transcript.find("Hello") != std::string::npos);
}
