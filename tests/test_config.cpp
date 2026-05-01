#include "config/config.hpp"

#include <doctest/doctest.h>

#include <variant>

using acva::config::Config;
using acva::config::LoadError;
using acva::config::load_from_string;
using acva::config::validate;

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
  file_path: /tmp/acva.log
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
    CHECK(*cfg.logging.file_path == "/tmp/acva.log");
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

// --- M2: supervisor + per-service health knobs ---

TEST_CASE("config: supervisor defaults") {
    auto r = load_from_string("logging: {}\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.supervisor.pipeline_fail_grace_seconds == 30);
    CHECK(cfg.supervisor.probe_timeout_ms == 3000);
    // Default health URLs are empty until the YAML provides them.
    CHECK(cfg.llm.health.health_url.empty());
    CHECK(cfg.stt.health.health_url.empty());
    CHECK(cfg.tts.health.health_url.empty());
    CHECK(cfg.llm.health.fail_pipeline_if_down);
    CHECK(cfg.llm.health.probe_interval_healthy_ms == 5000);
    CHECK(cfg.llm.health.probe_interval_degraded_ms == 1000);
    CHECK(cfg.llm.health.degraded_max_failures == 3);
}

TEST_CASE("config: supervisor + per-service health overrides") {
    constexpr auto yaml = R"(
logging: {}
control: {}
llm:
  health:
    health_url: "http://127.0.0.1:8081/health"
    fail_pipeline_if_down: true
    probe_interval_healthy_ms: 4000
    probe_interval_degraded_ms: 800
    degraded_max_failures: 2
stt:
  health:
    health_url: "http://127.0.0.1:8082/health"
    fail_pipeline_if_down: false
tts:
  health:
    health_url: ""
supervisor:
  pipeline_fail_grace_seconds: 15
  probe_timeout_ms: 2000
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.supervisor.pipeline_fail_grace_seconds == 15);
    CHECK(cfg.supervisor.probe_timeout_ms == 2000);
    CHECK(cfg.llm.health.health_url == "http://127.0.0.1:8081/health");
    CHECK(cfg.llm.health.probe_interval_healthy_ms == 4000);
    CHECK(cfg.llm.health.probe_interval_degraded_ms == 800);
    CHECK(cfg.llm.health.degraded_max_failures == 2);
    CHECK(cfg.stt.health.health_url == "http://127.0.0.1:8082/health");
    CHECK_FALSE(cfg.stt.health.fail_pipeline_if_down);
    CHECK(cfg.tts.health.health_url.empty());
}

TEST_CASE("config: zero probe intervals rejected when health_url set") {
    constexpr auto yaml = R"(
logging: {}
control: {}
llm:
  health:
    health_url: "http://127.0.0.1:8081/health"
    probe_interval_healthy_ms: 0
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("llm.health.probe_interval_healthy_ms")
          != std::string::npos);
}

TEST_CASE("config: empty health_url skips per-service validation") {
    constexpr auto yaml = R"(
logging: {}
control: {}
llm:
  health:
    health_url: ""
    probe_interval_healthy_ms: 0
)";
    // Even with bogus zero values, an empty URL means the supervisor
    // ignores this service entirely — validation must not flag it.
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
}

TEST_CASE("config: zero supervisor.probe_timeout_ms rejected") {
    constexpr auto yaml = R"(
logging: {}
control: {}
supervisor:
  probe_timeout_ms: 0
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("supervisor.probe_timeout_ms")
          != std::string::npos);
}
