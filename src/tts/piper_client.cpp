#include "tts/piper_client.hpp"

#include "supervisor/probe.hpp"   // re-uses parse_url() for authority/path split

#include <httplib.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace acva::tts {

namespace {

// Streaming WAV parser: ingests bytes as they arrive from httplib and
// hands int16 PCM samples back to the caller via the on_audio
// callback. Only supports the format Piper emits — PCM (format=1),
// mono, 16-bit. Anything else is reported as an error.
//
// State machine:
//   AwaitingHeader → bytes < 44 — buffering until we have the full RIFF/fmt/data
//                                  header chain.
//   StreamingPcm   → header parsed; subsequent bytes are int16le samples.
//   Errored        → header was malformed; further feed() calls are no-ops.
//
// We tolerate the odd case where Piper inserts extra `LIST`/`bext`
// chunks between `fmt ` and `data` by walking subchunks rather than
// assuming the canonical 44-byte header.
class WavStream {
public:
    enum class Status { AwaitingHeader, StreamingPcm, Errored };

    using OnFormat = std::function<void(int sample_rate_hz)>;
    using OnAudio  = std::function<void(std::span<const std::int16_t>)>;
    using OnError  = std::function<void(std::string)>;

    WavStream(OnFormat on_format, OnAudio on_audio, OnError on_error)
        : on_format_(std::move(on_format)),
          on_audio_(std::move(on_audio)),
          on_error_(std::move(on_error)) {}

    // Feed raw response bytes. Returns false if streaming has errored
    // and the caller should stop reading; true to keep going.
    bool feed(std::string_view data) {
        if (status_ == Status::Errored) return false;
        if (data.empty()) return true;
        if (status_ == Status::AwaitingHeader) {
            buf_.insert(buf_.end(), data.begin(), data.end());
            if (!try_parse_header()) {
                // Header still incomplete; wait for more bytes.
                return status_ != Status::Errored;
            }
            // try_parse_header may have left a tail of audio bytes in buf_
            // — feed those through the streaming path below.
            data = std::string_view{};   // already drained via tail handling
        }
        if (status_ == Status::StreamingPcm) {
            // Accumulate any odd byte from a previous chunk. Audio data
            // is little-endian int16 — pairs of bytes.
            if (!odd_byte_buffer_.empty() && !data.empty()) {
                odd_byte_buffer_.push_back(data.front());
                data.remove_prefix(1);
                emit_samples(std::string_view{odd_byte_buffer_.data(),
                                                odd_byte_buffer_.size()});
                odd_byte_buffer_.clear();
            }
            const std::size_t even = data.size() & ~std::size_t{1};
            if (even > 0) emit_samples(data.substr(0, even));
            if (data.size() > even) {
                odd_byte_buffer_.push_back(data.back());
            }
        }
        return status_ != Status::Errored;
    }

    void finish() {
        if (status_ == Status::AwaitingHeader && !buf_.empty()) {
            fail("response truncated before WAV header complete");
        }
        // Ignore a trailing odd byte — odd-length PCM streams are
        // malformed and Piper never produces them.
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    static std::uint32_t read_u32_le(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0])
             | static_cast<std::uint32_t>(p[1]) <<  8
             | static_cast<std::uint32_t>(p[2]) << 16
             | static_cast<std::uint32_t>(p[3]) << 24;
    }
    static std::uint16_t read_u16_le(const std::uint8_t* p) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(p[0])
          | static_cast<std::uint16_t>(p[1]) << 8);
    }

    bool try_parse_header() {
        const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(buf_.data());
        const std::size_t   n = buf_.size();

        // RIFF/WAVE: 12 bytes minimum.
        if (n < 12) return false;
        if (std::memcmp(b,     "RIFF", 4) != 0
         || std::memcmp(b + 8, "WAVE", 4) != 0) {
            fail("not a RIFF/WAVE response");
            return true;
        }

        std::size_t pos = 12;
        bool have_fmt = false;
        std::uint32_t sample_rate = 0;
        std::uint16_t channels = 0;
        std::uint16_t bits = 0;
        std::uint16_t format_tag = 0;

        while (pos + 8 <= n) {
            const std::uint32_t sz = read_u32_le(b + pos + 4);
            char id[5] = {0};
            std::memcpy(id, b + pos, 4);
            const std::size_t body = pos + 8;
            if (body + sz > n) {
                // Subchunk body not fully buffered yet.
                return false;
            }
            if (std::memcmp(id, "fmt ", 4) == 0) {
                if (sz < 16) { fail("fmt subchunk too small"); return true; }
                format_tag  = read_u16_le(b + body + 0);
                channels    = read_u16_le(b + body + 2);
                sample_rate = read_u32_le(b + body + 4);
                bits        = read_u16_le(b + body + 14);
                have_fmt    = true;
            } else if (std::memcmp(id, "data", 4) == 0) {
                if (!have_fmt) { fail("data subchunk before fmt"); return true; }
                if (format_tag != 1) {
                    fail("non-PCM WAV format (tag=" + std::to_string(format_tag) + ")");
                    return true;
                }
                if (channels != 1) {
                    fail("non-mono WAV (channels=" + std::to_string(channels) + ")");
                    return true;
                }
                if (bits != 16) {
                    fail("non-16-bit WAV (bits=" + std::to_string(bits) + ")");
                    return true;
                }
                // Header is committed. Stash the remaining bytes (start
                // of audio) and switch to streaming mode.
                std::vector<char> tail(buf_.begin()
                                          + static_cast<std::ptrdiff_t>(body),
                                       buf_.end());
                buf_.clear();
                if (on_format_) on_format_(static_cast<int>(sample_rate));
                status_ = Status::StreamingPcm;
                if (!tail.empty()) {
                    feed(std::string_view{tail.data(), tail.size()});
                }
                return true;
            }
            // Skip unknown subchunk (`LIST`, `bext`, padding byte).
            pos = body + sz + (sz & 1);
        }
        return false;   // need more bytes
    }

    void emit_samples(std::string_view bytes) {
        if (bytes.empty()) return;
        // bytes.size() is guaranteed even at this point.
        const std::size_t n = bytes.size() / 2;
        std::vector<std::int16_t> samples(n);
        std::memcpy(samples.data(), bytes.data(), n * 2);
        if (on_audio_) on_audio_(std::span<const std::int16_t>{samples});
    }

    void fail(std::string msg) {
        if (status_ != Status::Errored) {
            status_ = Status::Errored;
            if (on_error_) on_error_(std::move(msg));
        }
    }

    OnFormat on_format_;
    OnAudio  on_audio_;
    OnError  on_error_;

    Status status_ = Status::AwaitingHeader;
    std::vector<char> buf_;
    std::string odd_byte_buffer_;
};

} // namespace

PiperClient::PiperClient(const config::TtsConfig& cfg) noexcept : cfg_(cfg) {}

std::string PiperClient::url_for(std::string_view lang) const {
    if (auto it = cfg_.voices.find(std::string{lang}); it != cfg_.voices.end()) {
        return it->second.url;
    }
    if (auto it = cfg_.voices.find(cfg_.fallback_lang); it != cfg_.voices.end()) {
        return it->second.url;
    }
    return {};
}

bool PiperClient::probe(std::string_view lang) {
    const auto url = url_for(lang);
    if (url.empty()) return false;
    auto parsed = supervisor::parse_url(url);
    if (parsed.authority.empty()) return false;
    httplib::Client cli(parsed.authority);
    cli.set_connection_timeout(std::chrono::seconds(2));
    cli.set_read_timeout(std::chrono::seconds(2));
    // Piper responds 405 to a HEAD on `/`, but the server is alive — we
    // only care that we got *something* HTTP-shaped back.
    auto res = cli.Head(parsed.path);
    return static_cast<bool>(res);
}

void PiperClient::submit(TtsRequest req, TtsCallbacks cb) {
    auto deliver_error = [&](std::string msg) {
        if (cb.on_error) cb.on_error(std::move(msg));
    };

    if (req.cancel && req.cancel->is_cancelled()) {
        deliver_error("cancelled");
        return;
    }

    const auto url = url_for(req.lang);
    if (url.empty()) {
        deliver_error("no voice configured for lang '" + req.lang
                       + "' and no fallback");
        return;
    }
    auto parsed = supervisor::parse_url(url);
    if (parsed.authority.empty()) {
        deliver_error("invalid voice url: " + url);
        return;
    }

    httplib::Client cli(parsed.authority);
    cli.set_connection_timeout(
        std::chrono::seconds(cfg_.request_timeout_seconds));
    cli.set_read_timeout(
        std::chrono::seconds(cfg_.request_timeout_seconds));

    // Piper's API: POST `/` with a JSON body { "text": "..." }. The
    // upstream server also accepts `text/plain`, but JSON is more
    // forgiving with embedded quotes and newlines that sentences can
    // contain. Mid-response cancellation isn't supported by httplib's
    // Post API (no ContentReceiver overload) — but for M3 it doesn't
    // matter: a single-sentence WAV is sub-second, and the playback
    // queue drops stale audio by turn id when a barge-in invalidates
    // the active turn (PlaybackQueue::invalidate_before in M3.3).
    std::string body = R"({"text":")";
    for (char c : req.text) {
        switch (c) {
            case '"':  body += "\\\""; break;
            case '\\': body += "\\\\"; break;
            case '\n': body += "\\n"; break;
            case '\r': body += "\\r"; break;
            case '\t': body += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[7];
                    std::snprintf(esc, sizeof esc, "\\u%04x",
                                   static_cast<unsigned>(static_cast<unsigned char>(c)));
                    body += esc;
                } else {
                    body += c;
                }
        }
    }
    body += "\"}";

    auto res = cli.Post(parsed.path, body, "application/json");

    if (req.cancel && req.cancel->is_cancelled()) {
        deliver_error("cancelled");
        return;
    }
    if (!res) {
        deliver_error(std::string{"http: "} + httplib::to_string(res.error()));
        return;
    }
    if (res->status < 200 || res->status >= 300) {
        deliver_error("http " + std::to_string(res->status));
        return;
    }

    // Stream the response body through the WAV parser. on_format /
    // on_audio fire from inside feed(); a malformed header surfaces
    // through deliver_error and we bail before on_finished.
    bool wav_errored = false;
    WavStream wav(
        cb.on_format,
        cb.on_audio,
        [&](std::string m) {
            wav_errored = true;
            deliver_error("wav: " + std::move(m));
        });
    wav.feed(std::string_view{res->body.data(), res->body.size()});
    if (wav_errored) return;
    wav.finish();
    if (wav.status() != WavStream::Status::StreamingPcm) {
        deliver_error("wav: no audio data in response");
        return;
    }
    if (cb.on_finished) cb.on_finished();
}

} // namespace acva::tts
