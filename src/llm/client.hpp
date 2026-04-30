#pragma once

#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/event.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace lclva::event { class EventBus; }

namespace lclva::llm {

// Outcome of a single LlmClient::submit() invocation. The DialogueManager
// translates this into an event::LlmFinished bus event.
struct LlmFinish {
    dialogue::TurnId turn = event::kNoTurn;
    bool cancelled = false;
    bool error = false;
    std::string error_message;       // diagnostic only — never spoken to user
    std::size_t tokens_generated = 0;
};

// LlmRequest carries the body produced by PromptBuilder verbatim. The
// PromptBuilder already baked in model / temperature / max_tokens /
// stream:true, so this struct is a thin wrapper around that body plus
// the per-turn cancellation handle and metadata for logging.
struct LlmRequest {
    std::string body;                // /v1/chat/completions JSON body
    std::shared_ptr<dialogue::CancellationToken> cancel;
    dialogue::TurnId turn = event::kNoTurn;
    std::string lang;                // for logging only
};

struct LlmCallbacks {
    std::function<void(std::string_view delta)> on_token;  // streamed content slice
    std::function<void(LlmFinish)>              on_finished;
};

// LlmClient — libcurl-backed SSE streamer for /v1/chat/completions plus
// a non-streaming /health probe via cpp-httplib.
//
// M1: submit() blocks the calling thread for the duration of the stream;
// callbacks fire inline on that thread. The DialogueManager calls submit()
// from its bus-subscriber thread, so the "I/O thread" referenced in
// project_design.md §16 is just whichever thread is dispatching the
// dialogue subscription. Reentrancy / a dedicated I/O thread arrives
// in M5 with speculative LLM.
//
// Cancellation: the per-turn CancellationToken is consulted inside the
// libcurl write callback. When set, the callback returns 0 which causes
// curl_easy_perform to abort with CURLE_WRITE_ERROR — observed and
// translated back into LlmFinish{cancelled=true} within < 100 ms.
class LlmClient {
public:
    LlmClient(const config::Config& cfg, event::EventBus& bus);
    ~LlmClient();

    LlmClient(const LlmClient&)            = delete;
    LlmClient& operator=(const LlmClient&) = delete;

    // Synchronous SSE stream. on_token fires per delta; on_finished fires
    // exactly once. Safe to be called serially from any thread; not
    // reentrant in M1.
    void submit(LlmRequest req, LlmCallbacks cb);

    // Non-streaming GET /health. Returns true on HTTP 200 within timeout.
    [[nodiscard]] bool probe();

    // Synchronous one-token completion to keep the LLM warm. M2 will
    // call this on a timer; M1 ships the machinery.
    void keep_alive();

private:
    const config::Config& cfg_;
    event::EventBus& bus_;
};

} // namespace lclva::llm
