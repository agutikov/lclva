#include "config/config.hpp"
#include "event/event.hpp"
#include "log/log.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <unistd.h>

namespace lcfg = acva::config;
namespace llog = acva::log;
namespace lev  = acva::event;

namespace {

// Run `body` with stderr redirected to a temp file; return what was written.
std::string capture_stderr(auto body) {
    std::FILE* tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);
    fflush(stderr);
    int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));

    body();

    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    std::rewind(tmp);
    std::ostringstream out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) {
        out.write(buf, static_cast<std::streamsize>(n));
    }
    std::fclose(tmp);
    return out.str();
}

void init_json_logger() {
    lcfg::LoggingConfig cfg;
    cfg.level = "info";
    cfg.sink = "stderr";
    llog::init(cfg);
}

} // namespace

TEST_CASE("json_sink: plain info emits {\"msg\":...}") {
    init_json_logger();
    auto out = capture_stderr([] {
        llog::info("dialogue", "fsm idle -> listening");
        llog::logger()->flush();
    });
    REQUIRE_FALSE(out.empty());
    CHECK(out.find("\"level\":\"info\"")              != std::string::npos);
    CHECK(out.find("\"component\":\"dialogue\"")     != std::string::npos);
    CHECK(out.find("\"msg\":\"fsm idle -> listening\"") != std::string::npos);
    // No structured-event fields on plain info.
    CHECK(out.find("\"event\":")                     == std::string::npos);
    CHECK(out.find("\"turn_id\":")                   == std::string::npos);
}

TEST_CASE("json_sink: log::event embeds structured fields") {
    init_json_logger();
    auto out = capture_stderr([] {
        llog::event("dialogue", "llm_first_token", 42,
                    {{"latency_ms", "381"}, {"lang", "en"}});
        llog::logger()->flush();
    });
    REQUIRE_FALSE(out.empty());
    CHECK(out.find("\"component\":\"dialogue\"")       != std::string::npos);
    CHECK(out.find("\"event\":\"llm_first_token\"")    != std::string::npos);
    CHECK(out.find("\"turn_id\":42")                    != std::string::npos);
    CHECK(out.find("\"latency_ms\":\"381\"")            != std::string::npos);
    CHECK(out.find("\"lang\":\"en\"")                    != std::string::npos);
    CHECK(out.find("\"msg\":")                          == std::string::npos);
}

TEST_CASE("json_sink: special characters in messages are JSON-escaped") {
    init_json_logger();
    auto out = capture_stderr([] {
        llog::info("test", "line\nbreak \"quote\" \\bs");
        llog::logger()->flush();
    });
    CHECK(out.find("\\n")       != std::string::npos);
    CHECK(out.find("\\\"quote\\\"") != std::string::npos);
    CHECK(out.find("\\\\bs")    != std::string::npos);
}

TEST_CASE("json_sink: kNoTurn omits the turn_id field") {
    init_json_logger();
    auto out = capture_stderr([] {
        llog::event("metrics", "snapshot", lev::kNoTurn,
                    {{"published", "17"}});
        llog::logger()->flush();
    });
    CHECK(out.find("\"event\":\"snapshot\"") != std::string::npos);
    CHECK(out.find("\"turn_id\":")            == std::string::npos);
    CHECK(out.find("\"published\":\"17\"")    != std::string::npos);
}

TEST_CASE("json_sink: every line is a valid standalone JSON object") {
    init_json_logger();
    auto out = capture_stderr([] {
        llog::info("a", "first");
        llog::info("b", "second");
        llog::event("c", "third", 7, {{"k", "v"}});
        llog::logger()->flush();
    });

    int objects = 0;
    std::istringstream s(out);
    std::string line;
    while (std::getline(s, line)) {
        if (line.empty()) continue;
        ++objects;
        // crude check: every line begins with { and ends with }.
        REQUIRE(line.front() == '{');
        REQUIRE(line.back()  == '}');
    }
    CHECK(objects == 3);
}
