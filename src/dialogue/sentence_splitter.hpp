#pragma once

#include "config/config.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lclva::dialogue {

// SentenceSplitter: streaming token-in / sentence-out.
//
// Spec (project_design.md §4.8):
//   - Emit a sentence only when:
//       a) terminal punctuation ('.','!','?') seen, AND
//       b) followed by whitespace+capital letter, OR end-of-stream, OR
//          chars-since-punctuation exceeds max_sentence_chars (forced flush).
//   - Skip punctuation immediately after known abbreviations (Dr., e.g., 3.14).
//   - Suppress splitting inside fenced code blocks (```); emit the whole
//     fenced block as a single sentence.
//   - Ellipses ('...', '…') are NOT sentence boundaries.
//   - List markers ('1.', '2.', 'a)') reset the boundary detector — the
//     '.' after the digit is not a sentence end.
//
// Multilingual punctuation extension (e.g. '。' for Japanese) is post-MVP.
// For M1 we handle ASCII '.', '!', '?' and the Unicode '…' shorthand.
//
// Usage:
//
//     SentenceSplitter sp(cfg);
//     std::vector<std::string> out;
//     sp.push("Hello, world. ", out);    // out: ["Hello, world."]
//     sp.push("How are ", out);          // out: []
//     sp.push("you?", out);              // out: []
//     sp.flush(out);                     // out: ["How are you?"]
//
class SentenceSplitter {
public:
    explicit SentenceSplitter(const config::SentenceSplitterConfig& cfg);

    // Append text; append zero-or-more complete sentences to `out`.
    void push(std::string_view text, std::vector<std::string>& out);

    // Stream-end: emit any non-empty buffered tail as a sentence.
    void flush(std::vector<std::string>& out);

    // Reset between turns (clears buffer + state).
    void reset();

    [[nodiscard]] std::size_t buffer_size() const noexcept { return buf_.size(); }

private:
    enum class Mode : std::uint8_t {
        Normal,
        InCodeFence, // inside a ``` ... ``` block
    };

    void flush_buffer(std::vector<std::string>& out);

    // When a terminator is deferred (decimal lookahead, ellipsis collapse,
    // or just one-char defer to see what follows), pending_term_pos_ holds
    // the index in buf_ of the deferred terminator. resolve_pending() splits
    // the buffer at pending_term_pos_+1, emitting the head as a sentence.
    bool resolve_pending(std::vector<std::string>& out);

    bool last_word_is_abbreviation() const;
    bool buffer_ends_with_list_marker() const;
    bool buffer_ends_with_ellipsis() const;
    bool buffer_ends_with_decimal() const;

    config::SentenceSplitterConfig cfg_;
    std::string buf_;
    Mode mode_ = Mode::Normal;
    std::size_t chars_since_terminator_ = 0;
    std::size_t pending_term_pos_ = std::string::npos;
    // Number of consecutive dots in the pending run. >=2 means ellipsis;
    // at resolve time we suppress the split and keep the dots in the
    // current sentence's buffer.
    std::size_t pending_dot_run_ = 0;

    // Lowercased abbreviations the splitter should NOT treat as sentence ends.
    // Stored without trailing dot; lookup compares the trailing word in buf_.
    std::unordered_set<std::string> abbreviations_;
};

} // namespace lclva::dialogue
