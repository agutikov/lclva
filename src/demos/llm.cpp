#include "demos/demo.hpp"

#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace acva::demos {

namespace {

// Build the smallest possible chat-completions JSON body. We don't go
// through PromptBuilder here — the demo is about verifying llama
// reachability + token streaming, not about memory assembly.
std::string build_simple_body(const config::Config& cfg, std::string_view prompt) {
    std::string out;
    out.reserve(256 + prompt.size());
    out += R"({"model":")"; out += cfg.llm.model; out += R"(",)";
    out += R"("messages":[{"role":"user","content":")";
    for (char c : prompt) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;
        }
    }
    out += R"("}],)";
    out += R"("temperature":)"; out += std::to_string(cfg.llm.temperature); out += ",";
    out += R"("max_tokens":40,"stream":true})";
    return out;
}

} // namespace

int run_llm(const config::Config& cfg, std::span<const std::string> /*args*/) {
    using namespace std::chrono;

    constexpr std::string_view kPrompt =
        "Reply with one short sentence acknowledging you are running locally.";

    std::printf("demo[llm] base_url='%s' model='%s'\n",
                 cfg.llm.base_url.c_str(), cfg.llm.model.c_str());

    event::EventBus bus;
    llm::LlmClient client(cfg, bus);

    if (!client.probe()) {
        std::fprintf(stderr,
            "demo[llm] FAIL: /health probe failed at %s — is llama-server up?\n",
            cfg.llm.base_url.c_str());
        return EXIT_FAILURE;
    }
    std::printf("demo[llm] /health ok; submitting prompt: \"%.*s\"\n\n  ",
                 static_cast<int>(kPrompt.size()), kPrompt.data());

    const auto t0 = steady_clock::now();
    std::atomic<bool> first_token_seen{false};
    std::chrono::steady_clock::time_point first_token_at;
    std::atomic<std::size_t> tokens{0};

    llm::LlmCallbacks cb;
    cb.on_token = [&](std::string_view delta) {
        if (!first_token_seen.exchange(true)) {
            first_token_at = steady_clock::now();
        }
        std::fwrite(delta.data(), 1, delta.size(), stdout);
        std::fflush(stdout);
        tokens.fetch_add(1, std::memory_order_relaxed);
    };
    cb.on_finished = [](llm::LlmFinish) {};

    auto cancel = std::make_shared<dialogue::CancellationToken>();
    client.submit(llm::LlmRequest{
        .body   = build_simple_body(cfg, kPrompt),
        .cancel = cancel,
        .turn   = 1,
        .lang   = "en",
    }, cb);

    const auto t1 = steady_clock::now();
    std::printf("\n");

    bus.shutdown();

    if (tokens.load() == 0) {
        std::fprintf(stderr,
            "demo[llm] FAIL: zero tokens received. The /health probe passed, "
            "but the chat-completions stream produced nothing — check the "
            "model alias (got '%s') against `docker compose logs llama`.\n",
            cfg.llm.model.c_str());
        return EXIT_FAILURE;
    }

    const auto first_ms = first_token_seen
        ? duration<double, std::milli>(first_token_at - t0).count()
        : -1.0;
    const auto total_ms = duration<double, std::milli>(t1 - t0).count();
    std::printf(
        "demo[llm] done: tokens=%zu first_token_ms=%.1f total_ms=%.1f\n",
        tokens.load(), first_ms, total_ms);
    return EXIT_SUCCESS;
}

} // namespace acva::demos
