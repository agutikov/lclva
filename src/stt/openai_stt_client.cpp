#include "stt/openai_stt_client.hpp"

#include "audio/wav.hpp"

#include <curl/curl.h>
#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>

namespace acva::stt {

namespace {

// libcurl write callback — accumulates the JSON response body.
std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb,
                        void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// Authority of cfg.stt.base_url — used to build /health URL.
std::string trim_to_authority(std::string_view url) {
    auto p = url.find("://");
    if (p == std::string_view::npos) return std::string{url};
    auto slash = url.find('/', p + 3);
    return std::string{slash == std::string_view::npos
                         ? url : url.substr(0, slash)};
}

struct TranscriptionResponse {
    std::string text;
    // Speaches honors ?response_format=verbose_json which adds
    // language + duration; we don't request it for M4B's request /
    // response client.
};

} // namespace

OpenAiSttClient::OpenAiSttClient(const config::SttConfig& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

OpenAiSttClient::~OpenAiSttClient() {
    curl_global_cleanup();
}

bool OpenAiSttClient::probe() {
    if (cfg_.base_url.empty()) return false;
    const std::string url = trim_to_authority(cfg_.base_url) + "/health";
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && status >= 200 && status < 300;
}

void OpenAiSttClient::submit(SttRequest req, SttCallbacks cb) {
    auto report_error = [&](std::string err) {
        if (cb.on_error) cb.on_error(std::move(err));
    };

    if (cfg_.base_url.empty()) {
        report_error("openai_stt: cfg.stt.base_url is empty");
        return;
    }
    if (cfg_.model.empty()) {
        report_error("openai_stt: cfg.stt.model is empty");
        return;
    }
    if (!req.slice) {
        report_error("openai_stt: empty audio slice");
        return;
    }
    if (req.cancel && req.cancel->is_cancelled()) {
        report_error("cancelled");
        return;
    }

    std::string url = cfg_.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/audio/transcriptions";

    const auto wav = audio::make_wav(req.slice->samples(),
                                       req.slice->sample_rate());

    const auto t0 = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl) { report_error("openai_stt: curl_easy_init failed"); return; }

    // Multipart form: file (WAV) + model + optional language.
    curl_mime* mime = curl_mime_init(curl);

    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filename(part, "utterance.wav");
    curl_mime_type(part, "audio/wav");
    curl_mime_data(part, wav.data(), wav.size());

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, cfg_.model.c_str(), CURL_ZERO_TERMINATED);

    if (!req.lang_hint.empty()) {
        part = curl_mime_addpart(mime);
        curl_mime_name(part, "language");
        curl_mime_data(part, req.lang_hint.c_str(), CURL_ZERO_TERMINATED);
    }

    std::string body;
    body.reserve(1024);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(cfg_.request_timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                       static_cast<long>(cfg_.request_timeout_seconds * 2));

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    if (req.cancel && req.cancel->is_cancelled()) {
        report_error("cancelled");
        return;
    }
    if (rc != CURLE_OK) {
        report_error(std::string{"openai_stt: curl: "} + curl_easy_strerror(rc));
        return;
    }
    if (status < 200 || status >= 300) {
        report_error(fmt::format("openai_stt: http {} from {}: {}",
                                  status, url, body.substr(0, 200)));
        return;
    }

    TranscriptionResponse parsed;
    constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};
    if (auto ec = glz::read<kReadOpts>(parsed, body); ec) {
        report_error(std::string{"openai_stt: response parse: "}
                      + glz::format_error(ec, body));
        return;
    }

    const auto t1 = std::chrono::steady_clock::now();

    if (cb.on_final) {
        cb.on_final(event::FinalTranscript{
            .turn                 = req.turn,
            .text                 = std::move(parsed.text),
            .lang                 = req.lang_hint,
            .confidence           = 0.0F,    // Speaches doesn't report it
            .audio_duration       = req.slice->duration(),
            .processing_duration  =
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0),
        });
    }
}

WarmupResult warmup(const config::SttConfig& cfg) {
    WarmupResult r;
    if (cfg.base_url.empty()) {
        r.error = "stt.base_url empty";
        return r;
    }
    constexpr std::uint32_t kRateHz = 16000;
    std::vector<std::int16_t> silence(kRateHz, std::int16_t{0});
    const auto now = std::chrono::steady_clock::now();
    auto fixture = std::make_shared<audio::AudioSlice>(
        std::move(silence), kRateHz, now, now);

    OpenAiSttClient client(cfg);
    SttCallbacks cb;
    cb.on_final = [](event::FinalTranscript) {};
    cb.on_error = [&](std::string e) { r.error = std::move(e); };

    const auto t0 = std::chrono::steady_clock::now();
    client.submit(SttRequest{
        .turn = event::kNoTurn,
        .slice = fixture,
        .cancel = std::make_shared<dialogue::CancellationToken>(),
        .lang_hint = cfg.language,
    }, cb);
    r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
    r.ok = r.error.empty();
    return r;
}

} // namespace acva::stt
