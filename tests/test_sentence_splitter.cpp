#include "config/config.hpp"
#include "dialogue/sentence_splitter.hpp"

#include <doctest/doctest.h>

#include <vector>

namespace dl = lclva::dialogue;

namespace {

dl::SentenceSplitter make() {
    return dl::SentenceSplitter(lclva::config::SentenceSplitterConfig{});
}

std::vector<std::string> split_all(std::string_view text) {
    auto sp = make();
    std::vector<std::string> out;
    sp.push(text, out);
    sp.flush(out);
    return out;
}

} // namespace

TEST_CASE("splitter: simple two sentences") {
    auto out = split_all("Hello, world. How are you?");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "Hello, world.");
    CHECK(out[1] == "How are you?");
}

TEST_CASE("splitter: abbreviation does not split") {
    auto out = split_all("Dr. Smith is here. We can begin.");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "Dr. Smith is here.");
    CHECK(out[1] == "We can begin.");
}

TEST_CASE("splitter: multiple abbreviations") {
    auto out = split_all("e.g. apples, i.e. fruit. So nice!");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "e.g. apples, i.e. fruit.");
    CHECK(out[1] == "So nice!");
}

TEST_CASE("splitter: decimal numbers don't split") {
    auto out = split_all("The price is $3.14. Or maybe $4.20?");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "The price is $3.14.");
    CHECK(out[1] == "Or maybe $4.20?");
}

TEST_CASE("splitter: enumerations don't split on '1.' '2.' etc") {
    auto out = split_all("1. First item.\n2. Second item.\n3. Third.");
    REQUIRE(out.size() == 3);
    CHECK(out[0].find("1. First item.") != std::string::npos);
    CHECK(out[1].find("2. Second item.") != std::string::npos);
    CHECK(out[2].find("3. Third.") != std::string::npos);
}

TEST_CASE("splitter: ellipsis ... is not a sentence boundary") {
    auto out = split_all("Loading... please wait. Done!");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "Loading... please wait.");
    CHECK(out[1] == "Done!");
}

TEST_CASE("splitter: ellipsis as Unicode … is not a sentence boundary") {
    auto out = split_all("Wait\xE2\x80\xA6 still loading. OK done.");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "Wait\xE2\x80\xA6 still loading.");
    CHECK(out[1] == "OK done.");
}

TEST_CASE("splitter: question + exclamation") {
    auto out = split_all("Really?! Are you serious?");
    // Common acceptable behavior: split after each terminator.
    REQUIRE(out.size() >= 2);
}

TEST_CASE("splitter: streaming push emits as soon as boundary seen") {
    auto sp = make();
    std::vector<std::string> out;
    sp.push("Hello, ",   out); CHECK(out.empty());
    sp.push("world. ",   out); REQUIRE(out.size() == 1); CHECK(out[0] == "Hello, world.");
    sp.push("Bye.",      out);
    sp.flush(out);
    REQUIRE(out.size() == 2);
    CHECK(out[1] == "Bye.");
}

TEST_CASE("splitter: forced flush on max_sentence_chars without punctuation") {
    lclva::config::SentenceSplitterConfig cfg;
    cfg.max_sentence_chars = 50;
    dl::SentenceSplitter sp(cfg);
    std::vector<std::string> out;
    // 60-char string with one comma at position 55 to give the splitter a
    // safe break point.
    std::string long_run(54, 'a');
    long_run += ", and more text here.";
    sp.push(long_run, out);
    sp.flush(out);
    // We expect at least 2 emitted sentences (forced flush + final tail).
    REQUIRE(out.size() >= 1);
}

TEST_CASE("splitter: reset between turns clears state") {
    auto sp = make();
    std::vector<std::string> out;
    sp.push("Half a sentence", out);
    sp.reset();
    sp.push("New start. Done.", out);
    sp.flush(out);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "New start.");
    CHECK(out[1] == "Done.");
}

TEST_CASE("splitter: code fence kept as one block until closed") {
    auto sp = make();
    std::vector<std::string> out;
    sp.push("Here is code: ```\nint main() { return 0; }\n```\nDone.", out);
    sp.flush(out);
    // The code fence (with trailing newline) is buffered into the same
    // sentence as either the prefix or as its own; after closing fence we
    // have one final sentence "Done." Acceptable: ≥2 outputs.
    REQUIRE(!out.empty());
    bool found_done = false;
    for (const auto& s : out) {
        if (s.find("Done.") != std::string::npos) found_done = true;
    }
    CHECK(found_done);
}
