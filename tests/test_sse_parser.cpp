#include "llm/sse_parser.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace llm = acva::llm;

namespace {

struct ParseRecorder {
    llm::SseParser parser;
    std::vector<std::string> data_payloads;
    std::vector<std::string> comments;
    int done_count = 0;

    ParseRecorder() {
        parser.set_on_data([this](std::string_view p)    { data_payloads.emplace_back(p); });
        parser.set_on_done([this]()                       { ++done_count; });
        parser.set_on_comment([this](std::string_view l)  { comments.emplace_back(l); });
    }
};

} // namespace

TEST_CASE("sse: single complete event") {
    ParseRecorder r;
    r.parser.feed("data: hello\n\n");

    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "hello");
    CHECK(r.done_count == 0);
}

TEST_CASE("sse: multiple events in one feed") {
    ParseRecorder r;
    r.parser.feed("data: a\n\ndata: b\n\ndata: c\n\n");

    REQUIRE(r.data_payloads.size() == 3);
    CHECK(r.data_payloads[0] == "a");
    CHECK(r.data_payloads[1] == "b");
    CHECK(r.data_payloads[2] == "c");
}

TEST_CASE("sse: byte-by-byte feed reassembles correctly") {
    ParseRecorder r;
    const std::string stream = "data: foo\n\ndata: bar\n\n";
    for (char c : stream) {
        r.parser.feed(std::string_view{&c, 1});
    }
    REQUIRE(r.data_payloads.size() == 2);
    CHECK(r.data_payloads[0] == "foo");
    CHECK(r.data_payloads[1] == "bar");
}

TEST_CASE("sse: split mid-payload across feeds") {
    ParseRecorder r;
    r.parser.feed("data: hel");
    r.parser.feed("lo wor");
    r.parser.feed("ld\n\n");
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "hello world");
}

TEST_CASE("sse: [DONE] sentinel ends the stream") {
    ParseRecorder r;
    r.parser.feed("data: tok1\n\ndata: [DONE]\n\ndata: tok2\n\n");

    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "tok1");
    CHECK(r.done_count == 1);
}

TEST_CASE("sse: comment lines fire on_comment but not on_data") {
    ParseRecorder r;
    r.parser.feed(": keepalive\n\ndata: x\n\n");
    REQUIRE(r.comments.size() == 1);
    CHECK(r.comments[0] == " keepalive");
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "x");
}

TEST_CASE("sse: unknown field lines (event:, id:) are ignored") {
    ParseRecorder r;
    r.parser.feed("event: token\nid: 7\ndata: payload\n\n");
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "payload");
}

TEST_CASE("sse: payload with no leading space is accepted") {
    ParseRecorder r;
    r.parser.feed("data:nospace\n\n");
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "nospace");
}

TEST_CASE("sse: finish() drains a tail without trailing blank line") {
    ParseRecorder r;
    r.parser.feed("data: tail");
    CHECK(r.data_payloads.empty());
    r.parser.finish();
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "tail");
}

TEST_CASE("sse: bytes after [DONE] are ignored") {
    ParseRecorder r;
    r.parser.feed("data: a\n\ndata: [DONE]\n\n");
    r.parser.feed("data: ignored\n\n");
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == "a");
    CHECK(r.parser.done_seen());
}

TEST_CASE("sse: realistic OpenAI chunk passes through verbatim") {
    ParseRecorder r;
    constexpr std::string_view chunk =
        R"(data: {"choices":[{"delta":{"content":"Hi"}}]})" "\n\n";
    r.parser.feed(chunk);
    REQUIRE(r.data_payloads.size() == 1);
    CHECK(r.data_payloads[0] == R"({"choices":[{"delta":{"content":"Hi"}}]})");
}
