#include "llm/sse_parser.hpp"

namespace acva::llm {

namespace {

constexpr std::string_view kDataPrefix    = "data:";
constexpr std::string_view kDoneSentinel  = "[DONE]";

// Strip optional single space immediately after a field prefix.
std::string_view trim_leading_space(std::string_view s) noexcept {
    if (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

// SSE event boundary is a blank line, which means \n\n in our input.
// (We don't bother with \r\n\r\n: llama.cpp uses \n\n. If a peer sends
// CRLF, the trailing \r will end up inside the event text and harmlessly
// truncated by the data: parser below.)
std::size_t find_event_boundary(std::string_view buf) noexcept {
    return buf.find("\n\n");
}

} // namespace

void SseParser::feed(std::string_view bytes) {
    if (done_seen_) return;
    buffer_.append(bytes.data(), bytes.size());

    while (true) {
        std::string_view view{buffer_};
        const auto pos = find_event_boundary(view);
        if (pos == std::string_view::npos) break;

        emit_event(view.substr(0, pos));
        buffer_.erase(0, pos + 2);

        if (done_seen_) {
            buffer_.clear();
            return;
        }
    }
}

void SseParser::finish() {
    if (done_seen_) return;
    if (buffer_.empty()) return;
    emit_event(buffer_);
    buffer_.clear();
}

void SseParser::emit_event(std::string_view event_text) {
    // An SSE event is one or more lines terminated by \n. The last
    // `data:` line wins for our use; OpenAI / llama.cpp emit one data
    // line per event so we don't handle multi-line concat here.
    std::size_t cursor = 0;
    while (cursor < event_text.size()) {
        const auto eol = event_text.find('\n', cursor);
        const auto line_end = (eol == std::string_view::npos) ? event_text.size() : eol;
        std::string_view line = event_text.substr(cursor, line_end - cursor);
        cursor = line_end + 1;

        if (line.empty()) continue;
        if (line.front() == ':') {
            if (on_comment_) on_comment_(line.substr(1));
            continue;
        }
        if (line.size() >= kDataPrefix.size()
            && line.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
            auto payload = trim_leading_space(line.substr(kDataPrefix.size()));
            if (payload == kDoneSentinel) {
                done_seen_ = true;
                if (on_done_) on_done_();
                return;
            }
            if (on_data_) on_data_(payload);
            continue;
        }
        // event: / id: / unknown — ignore for M1.
    }
}

} // namespace acva::llm
