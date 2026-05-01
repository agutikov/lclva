// Unit tests for the M5 envelope reassembler that decodes Speaches'
// `partial_message` / `full_message` wrappers from the WebRTC
// `oai-events` data channel into raw OpenAI Realtime event JSON.
//
// The wire format is documented in
// `plans/milestones/m5_streaming_stt.md` (header) and the project
// memory note `project_m5_realtime_spike.md` (resolved facts §3).

#include "stt/realtime_envelope.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

using acva::stt::realtime::EnvelopeReassembler;
using acva::stt::realtime::base64_decode;
using acva::stt::realtime::base64_encode;
using acva::stt::realtime::build_input_audio_buffer_append_json;
using acva::stt::realtime::build_simple_event_json;

namespace {

// Encode `s` to base64 with the standard alphabet + padding.
std::string b64(std::string_view s) {
    static constexpr std::string_view alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < s.size(); i += 3) {
        const auto a = static_cast<unsigned char>(s[i]);
        const auto b = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
        const auto c = i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0;
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(a) << 16) |
            (static_cast<std::uint32_t>(b) <<  8) |
             static_cast<std::uint32_t>(c);
        out.push_back(alpha[(triple >> 18) & 0x3F]);
        out.push_back(alpha[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < s.size() ? alpha[(triple >>  6) & 0x3F] : '=');
        out.push_back(i + 2 < s.size() ? alpha[(triple      ) & 0x3F] : '=');
    }
    return out;
}

std::string full_msg(std::string_view id, std::string_view inner) {
    std::string s = R"({"id":")";
    s += id;
    s += R"(","type":"full_message","data":")";
    s += b64(inner);
    s += R"("})";
    return s;
}

std::string partial_msg(std::string_view id, std::string_view data,
                        std::size_t idx, std::size_t total) {
    std::string s = R"({"id":")";
    s += id;
    s += R"(","type":"partial_message","data":")";
    s += data;
    s += R"(","fragment_index":)";
    s += std::to_string(idx);
    s += R"(,"total_fragments":)";
    s += std::to_string(total);
    s += R"(})";
    return s;
}

} // namespace

TEST_CASE("base64_decode: empty, ascii, padding variants") {
    CHECK(base64_decode("").value() == "");
    CHECK(base64_decode("aGVsbG8=").value() == "hello");          // 1 pad
    CHECK(base64_decode("aGVsbG8h").value() == "hello!");         // 0 pad
    CHECK(base64_decode("aGVsbG8sIHdvcmxk").value() == "hello, world");
    CHECK(base64_decode("YQ==").value() == "a");                  // 2 pad
}

TEST_CASE("base64_encode round-trips through base64_decode") {
    const std::string_view cases[] = {
        std::string_view{""},
        std::string_view{"a"},
        std::string_view{"ab"},
        std::string_view{"abc"},
        std::string_view{"abcd"},
        std::string_view{"hello, world"},
        std::string_view{"\x00\x01\x02\xff", 4},
    };
    for (auto s : cases) {
        const std::string encoded = base64_encode(s);
        const auto decoded = base64_decode(encoded);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == std::string(s));
    }
}

TEST_CASE("build_input_audio_buffer_append_json shapes the OpenAI Realtime event") {
    const std::string j = build_input_audio_buffer_append_json("evt_abc", "AAAA");
    CHECK(j == R"({"event_id":"evt_abc","type":"input_audio_buffer.append","audio":"AAAA"})");
}

TEST_CASE("build_simple_event_json carries event_id + type only") {
    CHECK(build_simple_event_json("e1", "input_audio_buffer.commit")
          == R"({"event_id":"e1","type":"input_audio_buffer.commit"})");
    CHECK(build_simple_event_json("e2", "input_audio_buffer.clear")
          == R"({"event_id":"e2","type":"input_audio_buffer.clear"})");
}

TEST_CASE("base64_decode: malformed input returns nullopt") {
    CHECK_FALSE(base64_decode("aGVsbG8").has_value());            // not %4
    CHECK_FALSE(base64_decode("aGV*bG8=").has_value());           // bad char
    CHECK_FALSE(base64_decode("aGV=bG8=").has_value());           // pad mid-stream
}

TEST_CASE("EnvelopeReassembler: full_message round-trip") {
    EnvelopeReassembler r;
    auto got = r.feed(full_msg("evt_1", R"({"type":"session.created"})"));
    REQUIRE(got.has_value());
    CHECK(*got == R"({"type":"session.created"})");
    CHECK(r.pending_count() == 0);
}

TEST_CASE("EnvelopeReassembler: partial_message reassembles in order") {
    EnvelopeReassembler r;
    const std::string inner = R"({"type":"transcription.delta","delta":"hello"})";
    const std::string encoded = b64(inner);
    const std::size_t mid = encoded.size() / 2;

    auto out0 = r.feed(partial_msg("evt_2", encoded.substr(0, mid), 0, 2));
    CHECK_FALSE(out0.has_value());
    CHECK(r.pending_count() == 1);

    auto out1 = r.feed(partial_msg("evt_2", encoded.substr(mid), 1, 2));
    REQUIRE(out1.has_value());
    CHECK(*out1 == inner);
    CHECK(r.pending_count() == 0);
}

TEST_CASE("EnvelopeReassembler: partial_message reassembles out of order") {
    // SCTP guarantees in-order delivery in practice, but the indexer
    // must not depend on it — defense in depth.
    EnvelopeReassembler r;
    const std::string inner = R"({"type":"x","payload":"abcdefghij"})";
    const std::string encoded = b64(inner);
    const std::size_t third = encoded.size() / 3;

    auto a = encoded.substr(0, third);
    auto b = encoded.substr(third, third);
    auto c = encoded.substr(2 * third);

    CHECK_FALSE(r.feed(partial_msg("evt_3", c, 2, 3)).has_value());
    CHECK_FALSE(r.feed(partial_msg("evt_3", a, 0, 3)).has_value());
    auto done = r.feed(partial_msg("evt_3", b, 1, 3));
    REQUIRE(done.has_value());
    CHECK(*done == inner);
    CHECK(r.pending_count() == 0);
}

TEST_CASE("EnvelopeReassembler: interleaved events isolated by id") {
    EnvelopeReassembler r;
    const std::string inner_a = R"({"type":"a"})";
    const std::string inner_b = R"({"type":"b"})";
    const std::string enc_a   = b64(inner_a);
    const std::string enc_b   = b64(inner_b);
    const auto half_a = enc_a.size() / 2;
    const auto half_b = enc_b.size() / 2;

    CHECK_FALSE(r.feed(partial_msg("A", enc_a.substr(0, half_a), 0, 2)).has_value());
    CHECK_FALSE(r.feed(partial_msg("B", enc_b.substr(0, half_b), 0, 2)).has_value());
    CHECK(r.pending_count() == 2);

    auto done_b = r.feed(partial_msg("B", enc_b.substr(half_b), 1, 2));
    REQUIRE(done_b.has_value());
    CHECK(*done_b == inner_b);
    CHECK(r.pending_count() == 1);

    auto done_a = r.feed(partial_msg("A", enc_a.substr(half_a), 1, 2));
    REQUIRE(done_a.has_value());
    CHECK(*done_a == inner_a);
    CHECK(r.pending_count() == 0);
}

TEST_CASE("EnvelopeReassembler: duplicate fragment ignored") {
    EnvelopeReassembler r;
    const std::string inner = R"({"type":"dup_test"})";
    const std::string encoded = b64(inner);
    const auto mid = encoded.size() / 2;

    CHECK_FALSE(r.feed(partial_msg("dup", encoded.substr(0, mid), 0, 2)).has_value());
    // Re-deliver the same fragment — must not advance completion.
    CHECK_FALSE(r.feed(partial_msg("dup", encoded.substr(0, mid), 0, 2)).has_value());
    CHECK(r.pending_count() == 1);
    auto done = r.feed(partial_msg("dup", encoded.substr(mid), 1, 2));
    REQUIRE(done.has_value());
    CHECK(*done == inner);
}

TEST_CASE("EnvelopeReassembler: malformed envelopes drop silently") {
    EnvelopeReassembler r;
    CHECK_FALSE(r.feed("not json at all").has_value());
    CHECK_FALSE(r.feed(R"({"id":"x","type":"unknown_type","data":""})").has_value());
    // partial_message missing fragment_index
    CHECK_FALSE(r.feed(R"({"id":"x","type":"partial_message","data":"abcd","total_fragments":2})").has_value());
    // fragment_index out of range
    CHECK_FALSE(r.feed(partial_msg("y", "abcd", 5, 2)).has_value());
    CHECK(r.pending_count() == 0);
}

TEST_CASE("EnvelopeReassembler: total_fragments mismatch drops the partial") {
    EnvelopeReassembler r;
    CHECK_FALSE(r.feed(partial_msg("Z", "AAAA", 0, 2)).has_value());
    // Same id, different total_fragments — server protocol violation.
    CHECK_FALSE(r.feed(partial_msg("Z", "BBBB", 0, 3)).has_value());
    CHECK(r.pending_count() == 0);
}
