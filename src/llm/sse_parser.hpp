#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace acva::llm {

// SseParser — accumulates raw bytes from an SSE stream and emits one
// payload per `data: ...\n\n` block. Designed for the OpenAI chat
// completions streaming format (which is also what llama.cpp emits when
// `stream: true` is set).
//
// It does NOT parse the JSON payload — callers do that. The parser only
// concerns itself with the SSE framing (data: prefix, double-newline
// terminator, [DONE] sentinel).
//
// The parser is single-threaded and stateful: feed bytes in arrival
// order. Each completed event triggers one of three callbacks:
//
//   on_data(payload)  for a `data: <payload>\n\n` block (payload may be
//                     empty or any UTF-8 bytes).
//   on_done()          when the literal `[DONE]` payload is received.
//   on_comment(line)   for `:`-prefixed comment lines (heartbeats);
//                      optional, may be left unset.
//
// Lines other than `data:` / `:` are silently ignored — llama.cpp does
// occasionally emit `event:` and `id:` lines that we don't act on.
class SseParser {
public:
    using DataCallback    = std::function<void(std::string_view payload)>;
    using DoneCallback    = std::function<void()>;
    using CommentCallback = std::function<void(std::string_view line)>;

    SseParser() = default;

    void set_on_data(DataCallback cb)       { on_data_    = std::move(cb); }
    void set_on_done(DoneCallback cb)       { on_done_    = std::move(cb); }
    void set_on_comment(CommentCallback cb) { on_comment_ = std::move(cb); }

    // Feed the parser some bytes. May trigger zero or more callbacks.
    void feed(std::string_view bytes);

    // Drain any final data: payload that lacks a trailing \n\n.
    // Call once when the upstream has closed the connection.
    void finish();

    // Reset state between requests.
    void reset() noexcept {
        buffer_.clear();
        done_seen_ = false;
    }

    [[nodiscard]] bool done_seen() const noexcept { return done_seen_; }

private:
    void emit_event(std::string_view event_text);

    std::string buffer_;
    bool done_seen_ = false;
    DataCallback    on_data_;
    DoneCallback    on_done_;
    CommentCallback on_comment_;
};

} // namespace acva::llm
