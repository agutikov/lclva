#include "config/config.hpp"

#include <doctest/doctest.h>

#include <variant>

using lclva::config::Config;
using lclva::config::LoadError;
using lclva::config::load_from_string;
using lclva::config::validate;

TEST_CASE("config: minimal valid YAML parses with all defaults") {
    auto r = load_from_string("logging: {}\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.logging.level == "info");
    CHECK(cfg.logging.sink == "stderr");
    CHECK(cfg.control.bind == "127.0.0.1");
    CHECK(cfg.control.port == 9876);
}

TEST_CASE("config: parses overrides") {
    constexpr auto yaml = R"(
logging:
  level: debug
  sink: file
  file_path: /tmp/lclva.log
control:
  bind: 0.0.0.0
  port: 12345
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.logging.level == "debug");
    CHECK(cfg.logging.sink == "file");
    REQUIRE(cfg.logging.file_path.has_value());
    CHECK(*cfg.logging.file_path == "/tmp/lclva.log");
    CHECK(cfg.control.bind == "0.0.0.0");
    CHECK(cfg.control.port == 12345);
}

TEST_CASE("config: rejects invalid log level") {
    auto r = load_from_string("logging:\n  level: shouty\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("logging.level") != std::string::npos);
}

TEST_CASE("config: rejects invalid sink") {
    auto r = load_from_string("logging:\n  sink: pigeon\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("logging.sink") != std::string::npos);
}

TEST_CASE("config: file sink without file_path is rejected") {
    auto r = load_from_string("logging:\n  sink: file\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("file_path") != std::string::npos);
}

TEST_CASE("config: zero port is rejected") {
    auto r = load_from_string("logging: {}\ncontrol:\n  port: 0\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("port") != std::string::npos);
}

TEST_CASE("config: malformed YAML returns parse error") {
    auto r = load_from_string("logging: { level: info\ncontrol: { port: 99\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("config:") == 0);
}

TEST_CASE("validate: catches errors directly") {
    Config cfg;
    cfg.logging.level = "nope";
    auto err = validate(cfg);
    REQUIRE(err.has_value());
    CHECK(err->message.find("logging.level") != std::string::npos);
}
