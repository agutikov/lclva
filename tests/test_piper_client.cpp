#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "tts/piper_client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using acva::config::TtsConfig;
using acva::config::TtsVoice;
using acva::dialogue::CancellationToken;
using acva::tts::PiperClient;
using acva::tts::TtsCallbacks;
using acva::tts::TtsRequest;

namespace {

// Build a minimal RIFF/WAVE PCM mono int16 byte stream around `samples`
// at `rate` Hz. Matches what `piper.http_server` emits.
std::string make_wav(std::span<const std::int16_t> samples, std::uint32_t rate) {
    const std::uint32_t bytes_per_sample = 2;
    const std::uint32_t channels = 1;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size())
                                       * bytes_per_sample;
    const std::uint32_t fmt_chunk_bytes = 16;
    const std::uint32_t riff_payload    = 4 /* WAVE */
                                          + 8 + fmt_chunk_bytes
                                          + 8 + data_bytes;

    auto u32 = [](std::uint32_t v) {
        std::string s(4, '\0');
        for (int i = 0; i < 4; ++i) s[static_cast<std::size_t>(i)] =
            static_cast<char>((v >> (i * 8)) & 0xFF);
        return s;
    };
    auto u16 = [](std::uint16_t v) {
        std::string s(2, '\0');
        s[0] = static_cast<char>(v & 0xFF);
        s[1] = static_cast<char>((v >> 8) & 0xFF);
        return s;
    };

    std::string out;
    out.reserve(44 + data_bytes);
    out.append("RIFF");                     out.append(u32(riff_payload));
    out.append("WAVE");
    out.append("fmt ");                     out.append(u32(fmt_chunk_bytes));
    out.append(u16(1));                     // PCM
    out.append(u16(static_cast<std::uint16_t>(channels)));
    out.append(u32(rate));
    out.append(u32(rate * channels * bytes_per_sample));   // byte rate
    out.append(u16(static_cast<std::uint16_t>(channels * bytes_per_sample)));
    out.append(u16(16));                    // bits per sample
    out.append("data");                     out.append(u32(data_bytes));
    out.append(reinterpret_cast<const char*>(samples.data()), data_bytes);
    return out;
}

// Tiny per-test Piper-like server. Records every received body and
// returns a WAV whose body length / sample rate are configurable per
// instance (so tests can assert routing by port).
class FakePiper {
public:
    explicit FakePiper(std::uint32_t rate_hz, std::size_t sample_count = 256)
        : rate_(rate_hz), sample_count_(sample_count) {
        srv_.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard lk(mu_);
                last_body_ = req.body;
                ++posts_;
            }
            std::vector<std::int16_t> s(sample_count_);
            for (std::size_t i = 0; i < s.size(); ++i) {
                s[i] = static_cast<std::int16_t>((i * 8) - 1024);  // arbitrary pattern
            }
            res.status = 200;
            res.set_content(make_wav(s, rate_), "audio/wav");
        });
        port_ = srv_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this] { srv_.listen_after_bind(); });
        for (int i = 0; i < 200 && !srv_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ~FakePiper() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    [[nodiscard]] std::string last_body() const {
        std::lock_guard lk(mu_); return last_body_;
    }
    [[nodiscard]] int posts() const {
        std::lock_guard lk(mu_); return posts_;
    }
    [[nodiscard]] int port() const noexcept { return port_; }

    // Cause the server to start sleeping mid-handler (forces the client
    // to time out instead of receiving a body promptly).
    void enable_sleep(std::chrono::milliseconds d) {
        std::lock_guard lk(mu_); sleep_ = d;
    }

    void set_status(int s) { status_override_.store(s); }

private:
    httplib::Server srv_;
    std::thread thread_;
    int port_ = 0;
    std::uint32_t rate_;
    std::size_t sample_count_;

    mutable std::mutex mu_;
    std::string last_body_;
    int posts_ = 0;
    std::chrono::milliseconds sleep_{0};
    std::atomic<int> status_override_{0};
};

TtsConfig make_tts_cfg(std::initializer_list<std::pair<std::string, std::string>> voices,
                       std::string fallback = "en") {
    TtsConfig c;
    for (const auto& [lang, url] : voices) {
        c.voices[lang] = TtsVoice{.url = url};
    }
    c.fallback_lang = std::move(fallback);
    c.request_timeout_seconds = 5;
    return c;
}

struct Captured {
    std::vector<std::int16_t> samples;
    int sample_rate_hz = 0;
    bool finished = false;
    std::string error;
    std::mutex mu;

    TtsCallbacks make_callbacks() {
        return TtsCallbacks{
            .on_format = [this](int hz) {
                std::lock_guard lk(mu);
                sample_rate_hz = hz;
            },
            .on_audio = [this](std::span<const std::int16_t> chunk) {
                std::lock_guard lk(mu);
                samples.insert(samples.end(), chunk.begin(), chunk.end());
            },
            .on_finished = [this] {
                std::lock_guard lk(mu);
                finished = true;
            },
            .on_error = [this](std::string e) {
                std::lock_guard lk(mu);
                error = std::move(e);
            },
        };
    }
};

} // namespace

TEST_CASE("PiperClient::url_for: language match, fallback, and miss") {
    auto cfg = make_tts_cfg({
        {"en", "http://127.0.0.1:8083"},
        {"ru", "http://127.0.0.1:8084"},
    }, "en");
    PiperClient pc(cfg);
    CHECK(pc.url_for("en") == "http://127.0.0.1:8083");
    CHECK(pc.url_for("ru") == "http://127.0.0.1:8084");
    // Unknown language → fallback ("en").
    CHECK(pc.url_for("de") == "http://127.0.0.1:8083");

    // No voices configured at all → empty.
    auto empty_cfg = make_tts_cfg({}, "en");
    PiperClient empty(empty_cfg);
    CHECK(empty.url_for("en").empty());
}

TEST_CASE("PiperClient::submit: 200 WAV → on_format + on_audio + on_finished") {
    FakePiper piper(22050, 512);
    auto cfg = make_tts_cfg({{"en", piper.url()}});
    PiperClient pc(cfg);

    Captured cap;
    pc.submit(TtsRequest{
        .turn = 1, .seq = 0,
        .text = "hello world",
        .lang = "en",
        .cancel = std::make_shared<CancellationToken>(),
    }, cap.make_callbacks());

    CHECK(cap.error.empty());
    CHECK(cap.finished);
    CHECK(cap.sample_rate_hz == 22050);
    CHECK(cap.samples.size() == 512);
    // The request body is the JSON-escaped text we sent.
    CHECK(piper.last_body().find("hello world") != std::string::npos);
    CHECK(piper.posts() == 1);
}

TEST_CASE("PiperClient::submit: lang routes to the right URL") {
    FakePiper en(22050);
    FakePiper ru(16000);                // distinct sample rate to confirm routing
    auto cfg = make_tts_cfg({
        {"en", en.url()},
        {"ru", ru.url()},
    });
    PiperClient pc(cfg);

    Captured cap_en;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="hi",.lang="en",.cancel={}},
              cap_en.make_callbacks());
    CHECK(cap_en.sample_rate_hz == 22050);
    CHECK(en.posts() == 1);
    CHECK(ru.posts() == 0);

    Captured cap_ru;
    pc.submit(TtsRequest{.turn=1,.seq=1,.text="привет",.lang="ru",.cancel={}},
              cap_ru.make_callbacks());
    CHECK(cap_ru.sample_rate_hz == 16000);
    CHECK(en.posts() == 1);
    CHECK(ru.posts() == 1);
}

TEST_CASE("PiperClient::submit: unknown lang falls back to fallback_lang voice") {
    FakePiper en(22050);
    FakePiper ru(16000);
    auto cfg = make_tts_cfg({
        {"en", en.url()},
        {"ru", ru.url()},
    }, "ru");                            // fallback = ru
    PiperClient pc(cfg);

    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="hi",.lang="zz",.cancel={}},
              cap.make_callbacks());
    CHECK(cap.error.empty());
    CHECK(cap.finished);
    CHECK(cap.sample_rate_hz == 16000);  // hit the fallback voice
    CHECK(en.posts() == 0);
    CHECK(ru.posts() == 1);
}

TEST_CASE("PiperClient::submit: no voices at all → on_error, no on_finished") {
    auto cfg = make_tts_cfg({}, "en");
    PiperClient pc(cfg);
    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="x",.lang="en",.cancel={}},
              cap.make_callbacks());
    CHECK_FALSE(cap.finished);
    CHECK(cap.error.find("no voice configured") != std::string::npos);
}

TEST_CASE("PiperClient::submit: pre-cancelled token short-circuits, no http call") {
    FakePiper piper(22050);
    auto cfg = make_tts_cfg({{"en", piper.url()}});
    PiperClient pc(cfg);

    auto tok = std::make_shared<CancellationToken>();
    tok->cancel();

    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="x",.lang="en",.cancel=tok},
              cap.make_callbacks());
    CHECK(cap.error == "cancelled");
    CHECK_FALSE(cap.finished);
    CHECK(piper.posts() == 0);
}

TEST_CASE("PiperClient::submit: server 5xx is reported as error") {
    httplib::Server srv;
    srv.Post("/", [](const httplib::Request&, httplib::Response& res) {
        res.status = 500;
        res.set_content("upstream went away", "text/plain");
    });
    int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&]{ srv.listen_after_bind(); });

    auto cfg = make_tts_cfg({{"en", "http://127.0.0.1:" + std::to_string(port)}});
    PiperClient pc(cfg);
    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="x",.lang="en",.cancel={}},
              cap.make_callbacks());

    CHECK(cap.error.find("http 500") != std::string::npos);
    CHECK_FALSE(cap.finished);

    srv.stop();
    th.join();
}

TEST_CASE("PiperClient::submit: malformed (non-WAV) response surfaces wav error") {
    httplib::Server srv;
    srv.Post("/", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("not a riff", "audio/wav");
    });
    int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&]{ srv.listen_after_bind(); });

    auto cfg = make_tts_cfg({{"en", "http://127.0.0.1:" + std::to_string(port)}});
    PiperClient pc(cfg);
    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text="x",.lang="en",.cancel={}},
              cap.make_callbacks());

    CHECK(cap.error.find("wav:") != std::string::npos);
    CHECK_FALSE(cap.finished);

    srv.stop();
    th.join();
}

TEST_CASE("PiperClient::submit: text with newlines and quotes survives JSON escape") {
    FakePiper piper(22050);
    auto cfg = make_tts_cfg({{"en", piper.url()}});
    PiperClient pc(cfg);

    const std::string text = "She said \"hi\".\nThen left.\tDone.";
    Captured cap;
    pc.submit(TtsRequest{.turn=1,.seq=0,.text=text,.lang="en",.cancel={}},
              cap.make_callbacks());
    CHECK(cap.finished);

    auto body = piper.last_body();
    CHECK(body.find(R"(\"hi\")") != std::string::npos);
    CHECK(body.find(R"(\n)") != std::string::npos);
    CHECK(body.find(R"(\t)") != std::string::npos);
}
