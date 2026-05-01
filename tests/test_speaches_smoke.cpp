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

#include <doctest/doctest.h>
#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <string>

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
constexpr const char* kTinyStt    = "Systran/faster-whisper-tiny";
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
        {"model", kTinyStt, "",           ""},
    };
    auto res = c.Post("/v1/audio/transcriptions", items);
    REQUIRE(res);
    CHECK(res->status == 200);
    INFO("transcription body=" << res->body);
    // Tiny model may misspell "acva"; assert on a robust prefix.
    CHECK(res->body.find("Hello") != std::string::npos);
}
