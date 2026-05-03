#include "tts/openai_tts_client.hpp"

#include <curl/curl.h>
#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>

namespace acva::tts {

namespace {

// Per-request state passed to libcurl's write callback. Lives on the
// caller's stack for the duration of curl_easy_perform.
struct WriteCtx {
    const TtsCallbacks&                          cb;
    std::shared_ptr<dialogue::CancellationToken> cancel;
    bool                                         format_announced = false;
    // PCM is bare int16 mono 22050 Hz — no header, no negotiation
    // needed. We announce the rate once, on the first byte received.
    int  sample_rate = 22050;
    // Carry-byte: when the chunk size is odd we hold the trailing
    // byte until the next chunk arrives so the int16 view we hand to
    // on_audio is always frame-aligned.
    bool has_carry  = false;
    char carry_byte = 0;
};

// libcurl write callback. Runs on curl's easy thread (= caller of
// curl_easy_perform). Returns 0 to abort, which curl maps to
// CURLE_WRITE_ERROR.
//
// Realtime contract: forwards bytes to on_audio as they arrive. The
// only buffering is the 1-byte carry across odd-sized chunk
// boundaries.
std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb,
                        void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    if (ctx->cancel && ctx->cancel->is_cancelled()) return 0;

    const std::size_t total = size * nmemb;
    if (total == 0) return 0;

    if (!ctx->format_announced) {
        ctx->format_announced = true;
        if (ctx->cb.on_format) ctx->cb.on_format(ctx->sample_rate);
    }

    // Build a frame-aligned view. Walk: if we're carrying a byte from
    // last chunk, prepend it and emit one int16 from (carry, ptr[0]),
    // then process the rest of the chunk in pairs.
    std::size_t consumed = 0;

    if (ctx->has_carry) {
        if (total >= 1) {
            const std::int16_t s = static_cast<std::int16_t>(
                static_cast<std::uint8_t>(ctx->carry_byte) |
                (static_cast<std::uint8_t>(ptr[0]) << 8));
            std::array<std::int16_t, 1> one{s};
            if (ctx->cb.on_audio) ctx->cb.on_audio({one.data(), one.size()});
            ctx->has_carry = false;
            consumed       = 1;
        }
    }

    const std::size_t rem = total - consumed;
    const std::size_t pairs = rem / 2;
    if (pairs > 0 && ctx->cb.on_audio) {
        // Reinterpret the byte run as int16 little-endian. Speaches
        // emits PCM little-endian on x86_64 / AArch64 targets;
        // matches host endianness for our supported platforms.
        std::span<const std::int16_t> view(
            reinterpret_cast<const std::int16_t*>(ptr + consumed), pairs);
        ctx->cb.on_audio(view);
    }
    if ((rem % 2) == 1) {
        ctx->carry_byte = ptr[consumed + pairs * 2];
        ctx->has_carry  = true;
    }
    return total;
}

// libcurl progress callback — keeps cancellation responsive when the
// server stalls mid-stream. Fires periodically (~1 s); returning
// non-zero aborts the transfer.
int curl_progress(void* userdata, curl_off_t /*dt*/, curl_off_t /*dn*/,
                   curl_off_t /*ut*/, curl_off_t /*un*/) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    return (ctx->cancel && ctx->cancel->is_cancelled()) ? 1 : 0;
}

// JSON escape for the `input` text field. Matches piper_client's
// escape rules — same set of characters needs encoding.
std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[7];
                    std::snprintf(esc, sizeof esc, "\\u%04x",
                                   static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Native-speech baseline used to translate cfg.tts.tempo_wpm into the
// OpenAI-compat `speed` multiplier. Piper voices land near 160 wpm at
// speed=1.0; Kokoro is similar. The conversion is approximate — the
// actual cadence depends on the voice and on punctuation density —
// but it gives the user a consistent knob with familiar units.
constexpr double kNativeWpm = 160.0;

// Build the request body. Format is OpenAI-compatible:
//   { "model":..., "input":..., "voice":..., "speed":..., "response_format":"pcm" }
// `voice` is omitted when the route doesn't carry one (single-speaker
// Piper voices); Speaches accepts either form. `speed` is omitted when
// `tempo_wpm` is 0 (= native cadence).
std::string build_body(std::string_view model_id,
                        std::string_view voice_id,
                        std::string_view text,
                        std::uint32_t   tempo_wpm) {
    std::string body;
    body.reserve(text.size() + 160);
    body += R"({"model":")";
    body += json_escape(model_id);
    body += R"(","input":")";
    body += json_escape(text);
    body += R"(",)";
    if (!voice_id.empty()) {
        body += R"("voice":")";
        body += json_escape(voice_id);
        body += R"(",)";
    }
    if (tempo_wpm > 0) {
        const double speed = static_cast<double>(tempo_wpm) / kNativeWpm;
        body += fmt::format(R"("speed":{:.3f},)", speed);
    }
    body += R"("response_format":"pcm"})";
    return body;
}

} // namespace

OpenAiTtsClient::OpenAiTtsClient(const config::TtsConfig& cfg) : cfg_(cfg) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

OpenAiTtsClient::~OpenAiTtsClient() {
    curl_global_cleanup();
}

OpenAiTtsClient::VoiceRoute
OpenAiTtsClient::route_for(std::string_view lang) const {
    if (auto it = cfg_.voices.find(std::string{lang});
        it != cfg_.voices.end()) {
        return {it->second.model_id, it->second.voice_id};
    }
    if (auto it = cfg_.voices.find(cfg_.fallback_lang);
        it != cfg_.voices.end()) {
        return {it->second.model_id, it->second.voice_id};
    }
    return {};
}

bool OpenAiTtsClient::probe() {
    if (cfg_.base_url.empty()) return false;
    std::string url = cfg_.base_url;
    // base_url commonly ends in /v1; /health lives at the server root.
    auto path_at = url.find("/v1");
    if (path_at != std::string::npos) url.resize(path_at);
    if (!url.empty() && url.back() != '/') url += '/';
    url += "health";

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

void OpenAiTtsClient::submit(TtsRequest req, TtsCallbacks cb) {
    auto report_error = [&](std::string err) {
        if (cb.on_error) cb.on_error(std::move(err));
    };

    if (cfg_.base_url.empty()) {
        report_error("openai_tts: cfg.tts.base_url is empty");
        return;
    }
    auto route = route_for(req.lang);
    if (route.model_id.empty()) {
        report_error(fmt::format(
            "openai_tts: no voice configured for lang='{}' (fallback '{}' "
            "also unset)", req.lang, cfg_.fallback_lang));
        return;
    }
    if (req.cancel && req.cancel->is_cancelled()) {
        report_error("cancelled");
        return;
    }

    // Build the request URL by appending /audio/speech to the base.
    std::string url = cfg_.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/audio/speech";

    const std::string body = build_body(
        route.model_id, route.voice_id, req.text, cfg_.tempo_wpm);

    CURL* curl = curl_easy_init();
    if (!curl) {
        report_error("openai_tts: curl_easy_init failed");
        return;
    }
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/octet-stream");

    WriteCtx ctx{
        .cb         = cb,
        .cancel     = req.cancel,
        .format_announced = false,
        .sample_rate = 22050,
        .has_carry  = false,
        .carry_byte = 0,
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                       static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                       static_cast<long>(cfg_.request_timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                       static_cast<long>(cfg_.request_timeout_seconds * 5));

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (req.cancel && req.cancel->is_cancelled()) {
        report_error("cancelled");
        return;
    }
    if (rc != CURLE_OK) {
        report_error(std::string{"openai_tts: curl: "} + curl_easy_strerror(rc));
        return;
    }
    if (status < 200 || status >= 300) {
        report_error(fmt::format("openai_tts: http {} from {}", status, url));
        return;
    }
    if (cb.on_finished) cb.on_finished();
}

} // namespace acva::tts
