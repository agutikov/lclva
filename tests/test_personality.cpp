#include "config/config.hpp"

#include <doctest/doctest.h>

#include <variant>

using acva::config::Config;
using acva::config::LoadError;
using acva::config::load_from_string;

TEST_CASE("personality: overlay applies all fields when present") {
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy:   { id: "speaches-ai/piper-en_US-amy-medium",   voice: "amy" }
    ru-irina: { id: "speaches-ai/piper-ru_RU-irina-medium", voice: "irina" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
    ru: en-amy
  fallback_lang: en
  tempo_wpm: 200
llm:
  temperature: 0.7
dialogue:
  max_assistant_sentences: 6
  max_assistant_tokens: 400
  fallback_language: en
  system_prompts:
    en: "default-en"
    ru: "default-ru"
active_personality: ingenue
personalities:
  ingenue:
    description: "test ingenue"
    system_prompts:
      en: "ingenue-en"
      ru: "ingenue-ru"
    voices:
      en: en-amy
      ru: ru-irina
    tempo_wpm: 240
    llm:
      temperature: 0.95
      top_p: 0.95
      repeat_penalty: 1.10
    dialogue:
      max_assistant_sentences: 10
      max_assistant_tokens: 800
      max_sentence_chars: 250
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.dialogue.system_prompts.at("en") == "ingenue-en");
    CHECK(cfg.dialogue.system_prompts.at("ru") == "ingenue-ru");
    CHECK(cfg.tts.voices_resolved.at("ru").voice_id == "irina");
    CHECK(cfg.tts.tempo_wpm == 240);
    CHECK(cfg.llm.temperature == doctest::Approx(0.95));
    CHECK(cfg.llm.top_p == doctest::Approx(0.95));
    CHECK(cfg.llm.repeat_penalty == doctest::Approx(1.10));
    CHECK(cfg.dialogue.max_assistant_sentences == 10);
    CHECK(cfg.dialogue.max_assistant_tokens == 800);
    CHECK(cfg.dialogue.sentence_splitter.max_sentence_chars == 250);
}

TEST_CASE("personality: missing scalar fields keep top-level defaults") {
    // Only description + system_prompts provided. Top-level llm/tts
    // numbers must survive untouched.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy: { id: "speaches-ai/piper-en_US-amy-medium", voice: "amy" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
  fallback_lang: en
  tempo_wpm: 200
llm:
  temperature: 0.5
dialogue:
  fallback_language: en
  system_prompts:
    en: "default"
active_personality: bare
personalities:
  bare:
    description: "no overrides"
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.tts.tempo_wpm == 200);
    CHECK(cfg.llm.temperature == doctest::Approx(0.5));
    CHECK(cfg.llm.top_p == doctest::Approx(1.0));
    CHECK(cfg.llm.repeat_penalty == doctest::Approx(1.0));
    CHECK(cfg.dialogue.system_prompts.at("en") == "default");
}

TEST_CASE("personality: partial language coverage falls back to top-level map") {
    // Personality only defines `en`. The `ru` entry must survive
    // from the top-level dialogue.system_prompts and tts.voices.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy:   { id: "speaches-ai/piper-en_US-amy-medium",   voice: "amy" }
    ru-irina: { id: "speaches-ai/piper-ru_RU-irina-medium", voice: "irina" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
    ru: ru-irina
  fallback_lang: en
dialogue:
  fallback_language: en
  system_prompts:
    en: "default-en"
    ru: "default-ru"
active_personality: en_only
personalities:
  en_only:
    description: "english-only personality"
    system_prompts:
      en: "persona-en"
    voices:
      en: en-amy
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    // English: overlaid.
    CHECK(cfg.dialogue.system_prompts.at("en") == "persona-en");
    CHECK(cfg.tts.voices_resolved.at("en").voice_id == "amy");
    // Russian: untouched fallbacks.
    CHECK(cfg.dialogue.system_prompts.at("ru") == "default-ru");
    CHECK(cfg.tts.voices_resolved.at("ru").voice_id == "irina");
}

TEST_CASE("personality: unknown active_personality is rejected") {
    constexpr auto yaml = R"(
logging: {}
control: {}
active_personality: ghost
personalities:
  consultant:
    description: "real one"
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<LoadError>(r));
    auto& msg = std::get<LoadError>(r).message;
    CHECK(msg.find("active_personality") != std::string::npos);
    CHECK(msg.find("ghost") != std::string::npos);
}

TEST_CASE("personality: empty active_personality leaves top-level untouched") {
    // No active_personality field at all means the registry is purely
    // declarative — the top-level subsystem fields stand verbatim.
    constexpr auto yaml = R"(
logging: {}
control: {}
llm:
  temperature: 0.42
tts:
  tempo_wpm: 180
personalities:
  consultant:
    description: "exists but inactive"
    tempo_wpm: 999
    llm:
      temperature: 0.0
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.llm.temperature == doctest::Approx(0.42));
    CHECK(cfg.tts.tempo_wpm == 180);
}

TEST_CASE("personality: voice alias resolves through TTS registry after overlay") {
    // Critical: the overlay runs BEFORE alias resolution, so a
    // personality-supplied tts.voices alias must end up in
    // voices_resolved with the correct {model_id, voice_id} tuple.
    constexpr auto yaml = R"(
logging: {}
control: {}
models:
  tts:
    en-amy:    { id: "speaches-ai/piper-en_US-amy-medium",    voice: "amy" }
    ru-ruslan: { id: "speaches-ai/piper-ru_RU-ruslan-medium", voice: "ruslan" }
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: en-amy
    ru: en-amy
  fallback_lang: en
active_personality: bender
personalities:
  bender:
    description: "deep masculine voice"
    voices:
      en: en-amy
      ru: ru-ruslan
)";
    auto r = load_from_string(yaml);
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.tts.voices.at("ru") == "ru-ruslan");
    CHECK(cfg.tts.voices_resolved.at("ru").model_id
          == "speaches-ai/piper-ru_RU-ruslan-medium");
    CHECK(cfg.tts.voices_resolved.at("ru").voice_id == "ruslan");
}

TEST_CASE("llm: top_p out of range rejected") {
    auto r = load_from_string("logging: {}\ncontrol: {}\nllm:\n  top_p: 1.5\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("top_p") != std::string::npos);
}

TEST_CASE("llm: top_p zero rejected (would suppress all sampling)") {
    auto r = load_from_string("logging: {}\ncontrol: {}\nllm:\n  top_p: 0.0\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("top_p") != std::string::npos);
}

TEST_CASE("llm: repeat_penalty out of range rejected") {
    auto r = load_from_string(
        "logging: {}\ncontrol: {}\nllm:\n  repeat_penalty: 3.0\n");
    REQUIRE(std::holds_alternative<LoadError>(r));
    CHECK(std::get<LoadError>(r).message.find("repeat_penalty") != std::string::npos);
}

TEST_CASE("llm: top_p and repeat_penalty round-trip from YAML") {
    auto r = load_from_string(
        "logging: {}\ncontrol: {}\n"
        "llm:\n  top_p: 0.9\n  repeat_penalty: 1.15\n");
    REQUIRE(std::holds_alternative<Config>(r));
    auto& cfg = std::get<Config>(r);
    CHECK(cfg.llm.top_p == doctest::Approx(0.9));
    CHECK(cfg.llm.repeat_penalty == doctest::Approx(1.15));
}
