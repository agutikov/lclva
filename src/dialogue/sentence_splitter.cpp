#include "dialogue/sentence_splitter.hpp"

#include <utility>

namespace lclva::dialogue {

namespace {

constexpr std::string_view kFenceOpen = "```";

// Conservative English abbreviation list. Keep it short to avoid false
// suppressions; expand based on observed mis-splits in the golden corpus.
const std::vector<std::string> kAbbreviations = {
    "dr", "mr", "mrs", "ms", "prof", "sr", "jr",
    "e.g", "i.e", "etc", "vs", "vol", "no", "fig", "approx",
    "st",       // Saint, Street
    "ave", "blvd", "rd",
    "u.s", "u.k", "u.s.a",
    "a.m", "p.m",
};

bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_terminator(char c) noexcept { return c == '.' || c == '!' || c == '?'; }
bool is_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

char to_lower(char c) noexcept {
    return is_upper(c) ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string trim_leading(std::string s) {
    std::size_t i = 0;
    while (i < s.size() && is_whitespace(s[i])) ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

bool starts_with_at(const std::string& s, std::size_t pos, std::string_view needle) {
    if (pos + needle.size() > s.size()) return false;
    for (std::size_t i = 0; i < needle.size(); ++i) {
        if (s[pos + i] != needle[i]) return false;
    }
    return true;
}

} // namespace

SentenceSplitter::SentenceSplitter(const config::SentenceSplitterConfig& cfg)
    : cfg_(cfg) {
    abbreviations_.reserve(kAbbreviations.size());
    for (const auto& s : kAbbreviations) abbreviations_.insert(s);
}

void SentenceSplitter::reset() {
    buf_.clear();
    mode_ = Mode::Normal;
    chars_since_terminator_ = 0;
    pending_term_pos_ = std::string::npos;
    pending_dot_run_ = 0;
}

void SentenceSplitter::push(std::string_view text, std::vector<std::string>& out) {
    for (char c : text) {
        // Code-fence transition detection happens before we append so we can
        // observe the third backtick.
        const std::size_t pre_size = buf_.size();
        buf_ += c;

        if (cfg_.detect_code_fences && pre_size + 1 >= kFenceOpen.size()
            && starts_with_at(buf_, buf_.size() - kFenceOpen.size(), kFenceOpen)) {
            mode_ = (mode_ == Mode::Normal) ? Mode::InCodeFence : Mode::Normal;
        }

        if (mode_ == Mode::InCodeFence) {
            ++chars_since_terminator_;
            continue;
        }

        // Resolve a previously deferred terminator if we have one.
        if (pending_term_pos_ != std::string::npos) {
            const char prev_term = buf_[pending_term_pos_];
            if (c == '.' && prev_term == '.') {
                // Run of dots continues. Track length; ≥2 means ellipsis.
                ++pending_dot_run_;
                continue;
            }
            if (is_digit(c)) {
                // "3.<digit>" or "3.14<digit>" — confirmed decimal/version.
                pending_term_pos_ = std::string::npos;
                pending_dot_run_ = 0;
                ++chars_since_terminator_;
                continue;
            }
            // Any other follow-up char: time to decide.
            if (pending_dot_run_ >= 2) {
                // Run of 3+ dots: ellipsis. Don't split; keep buffering.
                pending_term_pos_ = std::string::npos;
                pending_dot_run_ = 0;
                ++chars_since_terminator_;
                continue;
            }
            // Single deferred terminator → real boundary. Split.
            (void)resolve_pending(out);
            // fall through; c is now the first char of the new sentence.
        }

        if (is_terminator(c)) {
            // Decide the cases that NEVER split. List-marker check goes
            // before decimal because both look at "digit before dot" but
            // list-marker is more specific (start-of-line + short word).
            if (last_word_is_abbreviation() || buffer_ends_with_list_marker()) {
                pending_term_pos_ = std::string::npos;
                pending_dot_run_ = 0;
                continue;
            }
            if (buffer_ends_with_decimal()) {
                pending_term_pos_ = buf_.size() - 1;
                pending_dot_run_ = 1;
                continue;
            }
            if (buffer_ends_with_ellipsis()) {
                // Defensive: run of dots already consumed as ellipsis above.
                pending_term_pos_ = std::string::npos;
                pending_dot_run_ = 0;
                continue;
            }
            // Defer the flush by one char so we can collapse "..." sequences
            // and re-evaluate after seeing the next char.
            pending_term_pos_ = buf_.size() - 1;
            pending_dot_run_ = (c == '.') ? 1 : 0;
            continue;
        }

        ++chars_since_terminator_;

        // Forced flush when the unpunctuated tail grows too long.
        if (chars_since_terminator_ >= cfg_.max_sentence_chars
            && (is_whitespace(c) || c == ',')) {
            flush_buffer(out);
        }
    }
}

void SentenceSplitter::flush(std::vector<std::string>& out) {
    pending_term_pos_ = std::string::npos;
    pending_dot_run_ = 0;
    if (!buf_.empty()) {
        flush_buffer(out);
    }
    chars_since_terminator_ = 0;
}

bool SentenceSplitter::resolve_pending(std::vector<std::string>& out) {
    // Split buf_ at pending_term_pos_+1. The chars [pending+1 .. end-1) become
    // the start of the next sentence.
    const auto pos = pending_term_pos_;
    pending_term_pos_ = std::string::npos;
    if (pos == std::string::npos) return false;

    // The current incoming char `c` was just appended; the buffer tail is
    // [pos+1 .. buf_.size()-1].
    std::string head(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos) + 1);
    std::string tail(buf_.begin() + static_cast<std::ptrdiff_t>(pos) + 1, buf_.end());
    buf_ = std::move(tail);
    chars_since_terminator_ = buf_.size();

    auto sentence = trim_leading(std::move(head));
    if (!sentence.empty()) out.push_back(std::move(sentence));
    return true;
}

void SentenceSplitter::flush_buffer(std::vector<std::string>& out) {
    auto sentence = trim_leading(std::move(buf_));
    buf_.clear();
    chars_since_terminator_ = 0;
    pending_term_pos_ = std::string::npos;
    pending_dot_run_ = 0;
    if (!sentence.empty()) {
        out.push_back(std::move(sentence));
    }
}

bool SentenceSplitter::last_word_is_abbreviation() const {
    if (buf_.size() < 2) return false;
    if (buf_.back() != '.') return false; // only '.' creates abbreviation ambiguity
    std::size_t end = buf_.size() - 1;    // index of '.'
    std::size_t i = end;
    while (i > 0) {
        const char c = buf_[i - 1];
        if (is_lower(c) || is_upper(c) || c == '.' || is_digit(c)) {
            --i;
        } else {
            break;
        }
    }
    if (i == end) return false;
    std::string word;
    word.reserve(end - i);
    for (std::size_t k = i; k < end; ++k) {
        word += to_lower(buf_[k]);
    }
    return abbreviations_.contains(word);
}

bool SentenceSplitter::buffer_ends_with_list_marker() const {
    if (buf_.size() < 2) return false;
    if (buf_.back() != '.') return false;
    std::size_t end = buf_.size() - 1;
    std::size_t i = end;
    while (i > 0) {
        const char c = buf_[i - 1];
        if (is_digit(c) || is_lower(c) || is_upper(c)) {
            --i;
        } else {
            break;
        }
    }
    if (i == end) return false;
    const std::size_t len = end - i;
    if (len < 1 || len > 3) return false;
    if (i > 0) {
        const char prev = buf_[i - 1];
        if (prev != '\n' && prev != ' ' && prev != '\t' && prev != '\r') return false;
    }
    return true;
}

bool SentenceSplitter::buffer_ends_with_ellipsis() const {
    if (buf_.size() >= 3 && buf_[buf_.size() - 1] == '.'
        && buf_[buf_.size() - 2] == '.' && buf_[buf_.size() - 3] == '.') {
        return true;
    }
    if (buf_.size() >= 3) {
        const unsigned char a = static_cast<unsigned char>(buf_[buf_.size() - 3]);
        const unsigned char b = static_cast<unsigned char>(buf_[buf_.size() - 2]);
        const unsigned char d = static_cast<unsigned char>(buf_[buf_.size() - 1]);
        if (a == 0xE2 && b == 0x80 && d == 0xA6) return true;
    }
    return false;
}

bool SentenceSplitter::buffer_ends_with_decimal() const {
    if (buf_.size() < 2) return false;
    if (buf_.back() != '.') return false;
    return is_digit(buf_[buf_.size() - 2]);
}

} // namespace lclva::dialogue
