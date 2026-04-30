#include "llm/client.hpp"

#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/sse_parser.hpp"
#include "log/log.hpp"

#include <glaze/glaze.hpp>
#include <httplib.h>
#include <curl/curl.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acva::llm {

namespace {

// Smallest possible struct that captures the field we care about from
// each `data: {...}` SSE chunk. Glaze ignores any other fields (role,
// finish_reason, model, ...).
struct SseDelta {
    // content is `null` on the opening role-only delta and absent on
    // some finish chunks — optional<> handles both cleanly.
    std::optional<std::string> content;
};
struct SseChoice {
    SseDelta delta;
};
struct SseChunk {
    std::vector<SseChoice> choices;
};

struct WriteCtx {
    SseParser&                            parser;
    const LlmCallbacks&                   cb;
    std::shared_ptr<dialogue::CancellationToken> cancel;
    std::size_t&                          tokens_generated;
};

// libcurl write callback — runs on the curl easy thread (= caller's
// thread for the duration of curl_easy_perform). Returns 0 to signal
// cancellation, which curl maps to CURLE_WRITE_ERROR.
std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    if (ctx->cancel && ctx->cancel->is_cancelled()) return 0;

    const std::size_t total = size * nmemb;
    ctx->parser.feed(std::string_view{ptr, total});
    return total;
}

// libcurl progress callback — fires periodically (≈ 1 s by default) even
// when no bytes are flowing. Returning non-zero aborts the transfer.
// Keeps cancellation responsive when the server stalls mid-stream.
int curl_progress(void* userdata, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                  curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    return (ctx->cancel && ctx->cancel->is_cancelled()) ? 1 : 0;
}

// Strip the path component from cfg.llm.base_url so we can talk to
// /health (which lives at the server root, not under /v1).
std::string authority_of(std::string_view url) {
    const auto scheme_end = url.find("://");
    const auto host_start = (scheme_end == std::string_view::npos) ? 0 : scheme_end + 3;
    const auto path_start = url.find('/', host_start);
    return std::string{(path_start == std::string_view::npos) ? url : url.substr(0, path_start)};
}

// Lifted to namespace scope because glaze reflection cannot see types
// declared inside a function body.
struct PingMessage { std::string role; std::string content; };
struct PingRequest {
    std::string model;
    std::vector<PingMessage> messages;
    int max_tokens = 1;
    bool stream = false;
};

// Build a small "ping" request body for keep_alive(). Used by M2.
std::string build_keep_alive_body(const config::Config& cfg) {
    PingRequest r{
        .model = cfg.llm.model,
        .messages = { PingMessage{.role = "user", .content = "ping"} },
        .max_tokens = 1,
        .stream = false,
    };
    std::string out;
    auto ec = glz::write_json(r, out);
    if (ec) out.clear();
    return out;
}

} // namespace

LlmClient::LlmClient(const config::Config& cfg, event::EventBus& bus)
    : cfg_(cfg), bus_(bus) {
    // libcurl global init is process-wide and idempotent. Safe to call
    // here; matched by global_cleanup in the destructor.
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

LlmClient::~LlmClient() {
    curl_global_cleanup();
}

void LlmClient::submit(LlmRequest req, LlmCallbacks cb) {
    LlmFinish finish{
        .turn = req.turn,
        .cancelled = false,
        .error = false,
        .error_message = {},
        .tokens_generated = 0,
    };

    SseParser parser;
    constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};
    parser.set_on_data([&](std::string_view payload) {
        SseChunk chunk;
        auto ec = glz::read<kReadOpts>(chunk, payload);
        if (ec) {
            // Malformed SSE chunk — log and skip; never surface to user.
            log::info("llm", std::string{"sse parse error: "}
                              + glz::format_error(ec, std::string{payload}));
            return;
        }
        if (chunk.choices.empty()) return;
        auto& delta = chunk.choices.front().delta;
        if (!delta.content || delta.content->empty()) return;
        ++finish.tokens_generated;
        if (cb.on_token) cb.on_token(*delta.content);
    });
    parser.set_on_done([&]() {
        // Sentinel reached; perform() will then return naturally.
    });

    WriteCtx ctx{
        .parser = parser,
        .cb = cb,
        .cancel = req.cancel,
        .tokens_generated = finish.tokens_generated,
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        finish.error = true;
        finish.error_message = "curl_easy_init failed";
        if (cb.on_finished) cb.on_finished(finish);
        return;
    }

    const std::string url = cfg_.llm.base_url + "/chat/completions";

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(req.body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(cfg_.llm.request_timeout_seconds));
    // No total-request timeout: streaming responses can legitimately run
    // longer than request_timeout_seconds. Cancellation is the right tool
    // for runaway generations.

    const CURLcode rc = curl_easy_perform(curl);
    parser.finish();

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    finish.tokens_generated = ctx.tokens_generated;

    if (req.cancel && req.cancel->is_cancelled()) {
        finish.cancelled = true;
    } else if (rc != CURLE_OK) {
        finish.error = true;
        finish.error_message = curl_easy_strerror(rc);
    } else if (http_status != 200) {
        finish.error = true;
        finish.error_message = "http " + std::to_string(http_status);
    }

    if (cb.on_finished) cb.on_finished(finish);

    if (finish.error) {
        log::info("llm",
            std::string{"submit failed turn="} + std::to_string(finish.turn)
            + " err=" + finish.error_message);
    }

    bus_.publish(event::LlmFinished{
        .turn             = finish.turn,
        .cancelled        = finish.cancelled,
        .tokens_generated = finish.tokens_generated,
    });
}

bool LlmClient::probe() {
    httplib::Client cli(authority_of(cfg_.llm.base_url));
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    auto res = cli.Get("/health");
    return res && res->status == 200;
}

void LlmClient::keep_alive() {
    httplib::Client cli(authority_of(cfg_.llm.base_url));
    cli.set_connection_timeout(2);
    cli.set_read_timeout(static_cast<time_t>(cfg_.llm.request_timeout_seconds));
    auto body = build_keep_alive_body(cfg_);
    if (body.empty()) return;
    (void)cli.Post("/v1/chat/completions", body, "application/json");
}

} // namespace acva::llm
