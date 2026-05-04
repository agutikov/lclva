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

// --- M3: tts.voices, audio, playback ---

TEST_CASE("config: M3 defaults") {
    auto r = load_from_string("logging: {}\ncontrol: {}\n");
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.tts.voices.empty());
    CHECK(cfg.tts.fallback_lang == "en");
    CHECK(cfg.tts.request_timeout_seconds == 10);
    CHECK(cfg.audio.output_device == "default");
    CHECK(cfg.audio.sample_rate_hz == 48000);
    CHECK(cfg.audio.buffer_frames == 480);
    CHECK(cfg.playback.max_queue_chunks == 0);   // 0 = unbounded (pre-M7)
    CHECK(cfg.playback.underrun_log_throttle_ms == 1000);
    CHECK(cfg.dialogue.max_tts_queue_sentences == 3);
}

TEST_CASE("config: tts.voices alias resolves through models.tts registry") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy:   { id: "speaches-ai/piper-en_US-amy-medium",    voice: "amy" }
    ru-irina: { id: "speaches-ai/piper-ru_RU-irina-medium",  voice: "irina" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
    ru: ru-irina
  fallback_lang: en
  request_timeout_seconds: 7
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    REQUIRE(cfg.tts.voices.size() == 2);
    REQUIRE(cfg.tts.voices_resolved.size() == 2);
    CHECK(cfg.tts.voices_resolved.at("en").model_id
          == "speaches-ai/piper-en_US-amy-medium");
    CHECK(cfg.tts.voices_resolved.at("en").voice_id == "amy");
    CHECK(cfg.tts.voices_resolved.at("ru").voice_id == "irina");
    CHECK(cfg.tts.fallback_lang == "en");
    CHECK(cfg.tts.request_timeout_seconds == 7);
}

TEST_CASE("config: tts.base_url without scheme rejected") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy: { id: "speaches-ai/piper-en_US-amy-medium", voice: "amy" }
tts:
  base_url: "127.0.0.1:8090/v1"
  voices:
    en: en-amy
  fallback_lang: en
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("tts.base_url")
          != std::string::npos);
    CHECK(std::get<LoadError>(r).message.find("scheme")
          != std::string::npos);
}

TEST_CASE("config: tts.fallback_lang must point to a configured voice") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    ru-irina: { id: "speaches-ai/piper-ru_RU-irina-medium", voice: "irina" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    ru: ru-irina
  fallback_lang: en
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("tts.fallback_lang")
          != std::string::npos);
}

TEST_CASE("config: empty tts.voices skips fallback validation") {
    // No TTS configured at all is a valid state — M3 path is just disabled.
    constexpr auto yaml = R"(
logging: {}
control: {}
tts:
  fallback_lang: zz
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
}

TEST_CASE("config: unknown tts.voices alias rejected") {
    // Aliases must be present in models.tts. Pre-registry configs that
    // wrote {model_id, voice_id} inline no longer parse; the registry
    // is the only path.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy: { id: "speaches-ai/piper-en_US-amy-medium", voice: "amy" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: nonexistent-alias
  fallback_lang: en
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("nonexistent-alias")
          != std::string::npos);
}

TEST_CASE("config: tts model registry entry without voice rejected") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy: { id: "speaches-ai/piper-en_US-amy-medium" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
  fallback_lang: en
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("models.tts") != std::string::npos);
}

TEST_CASE("config: tts.base_url required when tts.voices is non-empty") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy: { id: "speaches-ai/piper-en_US-amy-medium", voice: "amy" }
tts:
  voices:
    en: en-amy
  fallback_lang: en
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("tts.base_url") != std::string::npos);
}

TEST_CASE("config: stt.model alias resolves to full HF id") {
    // Round-trip: alias in cfg.stt.model is replaced with the registry's
    // full id by config::resolve_aliases() before validate runs.
    // Unknown values pass through unchanged for back-compat.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  stt:
    large-v3-turbo: { id: "deepdml/faster-whisper-large-v3-turbo-ct2" }
stt:
  base_url: "http://127.0.0.1:8090/v1"
  model: large-v3-turbo
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.stt.model == "deepdml/faster-whisper-large-v3-turbo-ct2");
}

TEST_CASE("config: unregistered stt.model passes through (back-compat)") {
    constexpr auto yaml = R"(
logging: {}
control: {}
stt:
  base_url: "http://127.0.0.1:8090/v1"
  model: "Custom/some-model"
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.stt.model == "Custom/some-model");
}

TEST_CASE("config: vad.model_path alias resolves to per-type subdir") {
    // Registry stores bare filename; resolver prepends `models/silero/`
    // so bootstrap.cpp's resolve_data_path lands on
    // ${XDG}/acva/models/silero/<file>.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  vad:
    silero-v5: { file: silero_vad.onnx }
vad:
  model_path: silero-v5
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.vad.model_path == "models/silero/silero_vad.onnx");
}

TEST_CASE("config: llm.model alias is preserved as endpoint label") {
    // LLM aliasing is metadata-only pre-M8A: cfg.llm.model stays the
    // alias name (which llama-server reports via /v1/models). The
    // resolved filename lives in models.llm.<alias>.file for M8A's
    // future model-controller wiring.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  llm:
    dialog: { file: qwen3-stage1.q4_k_m.gguf }
llm:
  base_url: "http://127.0.0.1:8081/v1"
  model: dialog
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.llm.model == "dialog");          // unchanged
    REQUIRE(cfg.models.llm.contains("dialog"));
    CHECK(cfg.models.llm.at("dialog").file == "qwen3-stage1.q4_k_m.gguf");
}

TEST_CASE("config: registry url + size fields parse") {
    // Required by tools/acva-models for direct downloads.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  llm:
    dialog:
      file:    qwen3-stage1.q4_k_m.gguf
      url:     https://example.com/qwen.gguf
      size:    5027783520
      purpose: "test entry"
  vad:
    silero-v5:
      file:    silero_vad.onnx
      url:     https://example.com/silero.onnx
      purpose: "test entry"
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    REQUIRE(cfg.models.llm.contains("dialog"));
    CHECK(cfg.models.llm.at("dialog").url == "https://example.com/qwen.gguf");
    CHECK(cfg.models.llm.at("dialog").size == 5027783520ULL);
    REQUIRE(cfg.models.vad.contains("silero-v5"));
    CHECK(cfg.models.vad.at("silero-v5").url == "https://example.com/silero.onnx");
}

TEST_CASE("config: zero audio.sample_rate_hz rejected") {
    constexpr auto yaml = R"(
logging: {}
control: {}
audio:
  sample_rate_hz: 0
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("audio.sample_rate_hz")
          != std::string::npos);
}

TEST_CASE("config: zero playback.max_queue_chunks accepted as unbounded") {
    // Pre-M7: max_queue_chunks=0 means "unbounded queue", letting
    // the full LLM monologue sit in RAM rather than dropping chunks
    // mid-sentence. The validator must accept it; the queue treats
    // 0 as "skip the cap check".
    constexpr auto yaml = R"(
logging: {}
control: {}
playback:
  max_queue_chunks: 0
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    const auto& cfg = std::get<Config>(r);
    CHECK(cfg.playback.max_queue_chunks == 0);
}
