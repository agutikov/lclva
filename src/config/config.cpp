#include "config/config.hpp"

#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace acva::config {

namespace {

constexpr std::array kValidLogLevels{
    std::string_view{"trace"}, std::string_view{"debug"}, std::string_view{"info"},
    std::string_view{"warn"},  std::string_view{"error"}, std::string_view{"critical"},
    std::string_view{"off"},
};

constexpr std::array kValidSinks{
    std::string_view{"stderr"},
    std::string_view{"journal"},
    std::string_view{"file"},
    std::string_view{"dir"},
};

bool contains(std::string_view value, const auto& valid_set) noexcept {
    for (auto v : valid_set) {
        if (v == value) {
            return true;
        }
    }
    return false;
}

std::string read_file(const std::filesystem::path& path, LoadError& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err.message = "config: cannot open file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

LogSink parse_log_sink(std::string_view s) {
    if (s == "journal") return LogSink::journal;
    if (s == "file")    return LogSink::file;
    if (s == "dir")     return LogSink::dir;
    return LogSink::stderr_;
}

std::optional<LoadError> validate(const Config& cfg) {
    if (!contains(cfg.logging.level, kValidLogLevels)) {
        return LoadError{"config: logging.level: invalid value '" + cfg.logging.level + "'"};
    }
    if (!contains(cfg.logging.sink, kValidSinks)) {
        return LoadError{"config: logging.sink: invalid value '" + cfg.logging.sink + "'"};
    }
    if (cfg.logging.sink == "file" && !cfg.logging.file_path) {
        return LoadError{"config: logging.sink='file' requires logging.file_path"};
    }
    if (cfg.logging.sink == "dir" && !cfg.logging.dir_path) {
        return LoadError{"config: logging.sink='dir' requires logging.dir_path"};
    }
    if (cfg.control.port == 0) {
        return LoadError{"config: control.port: must be non-zero"};
    }
    if (cfg.pipeline.fake_barge_in_probability < 0.0
        || cfg.pipeline.fake_barge_in_probability > 1.0) {
        return LoadError{"config: pipeline.fake_barge_in_probability: must be in [0, 1]"};
    }
    if (cfg.pipeline.fake_sentences_per_turn == 0) {
        return LoadError{"config: pipeline.fake_sentences_per_turn: must be > 0"};
    }
    if (cfg.llm.temperature < 0.0 || cfg.llm.temperature > 2.0) {
        return LoadError{"config: llm.temperature: must be in [0, 2]"};
    }
    if (cfg.llm.max_tokens == 0) {
        return LoadError{"config: llm.max_tokens: must be > 0"};
    }
    if (cfg.llm.max_prompt_tokens == 0) {
        return LoadError{"config: llm.max_prompt_tokens: must be > 0"};
    }
    if (cfg.memory.recent_turns_n == 0) {
        return LoadError{"config: memory.recent_turns_n: must be > 0"};
    }
    static constexpr std::array kSummaryTriggers{
        std::string_view{"turns"}, std::string_view{"tokens"},
        std::string_view{"idle"},  std::string_view{"hybrid"},
    };
    if (!contains(cfg.memory.summary.trigger, kSummaryTriggers)) {
        return LoadError{"config: memory.summary.trigger: must be one of turns|tokens|idle|hybrid"};
    }
    static constexpr std::array kFactsPolicies{
        std::string_view{"conservative"}, std::string_view{"moderate"},
        std::string_view{"aggressive"},   std::string_view{"manual_only"},
    };
    if (!contains(cfg.memory.facts.policy, kFactsPolicies)) {
        return LoadError{"config: memory.facts.policy: must be conservative|moderate|aggressive|manual_only"};
    }
    if (cfg.dialogue.max_assistant_sentences == 0) {
        return LoadError{"config: dialogue.max_assistant_sentences: must be > 0"};
    }

    // Per-service health probes. Empty health_url disables a service's
    // probe entirely — no need to validate the rest of the knobs in that
    // case. Each tuple: (logical name, ServiceHealthConfig ref).
    using Hpair = std::pair<std::string_view, const ServiceHealthConfig*>;
    const std::array<Hpair, 3> health_sections{{
        {"llm", &cfg.llm.health},
        {"stt", &cfg.stt.health},
        {"tts", &cfg.tts.health},
    }};
    for (const auto& [name, h] : health_sections) {
        if (h->health_url.empty()) continue;
        if (h->probe_interval_healthy_ms == 0) {
            return LoadError{std::string{"config: "} + std::string{name}
                + ".health.probe_interval_healthy_ms: must be > 0"};
        }
        if (h->probe_interval_degraded_ms == 0) {
            return LoadError{std::string{"config: "} + std::string{name}
                + ".health.probe_interval_degraded_ms: must be > 0"};
        }
        if (h->degraded_max_failures == 0) {
            return LoadError{std::string{"config: "} + std::string{name}
                + ".health.degraded_max_failures: must be > 0"};
        }
    }
    if (cfg.supervisor.probe_timeout_ms == 0) {
        return LoadError{"config: supervisor.probe_timeout_ms: must be > 0"};
    }
    // Only enforce fallback_language presence when *any* system_prompts are
    // configured. An empty map is allowed for tests / minimal configs;
    // dialogue.manager will error at runtime if it actually tries to look one up.
    if (!cfg.dialogue.system_prompts.empty()
        && !cfg.dialogue.system_prompts.contains(cfg.dialogue.fallback_language)) {
        return LoadError{
            "config: dialogue.system_prompts: must contain an entry for the fallback_language ('"
            + cfg.dialogue.fallback_language + "')"};
    }

    // M3 — TTS / audio / playback knobs.
    if (cfg.tts.request_timeout_seconds == 0) {
        return LoadError{"config: tts.request_timeout_seconds: must be > 0"};
    }
    if (cfg.tts.tempo_wpm != 0
        && (cfg.tts.tempo_wpm < 50 || cfg.tts.tempo_wpm > 400)) {
        return LoadError{
            "config: tts.tempo_wpm: must be 0 or in [50, 400]"};
    }
    // M4B — STT.
    if (!cfg.stt.base_url.empty()) {
        if (cfg.stt.base_url.find("://") == std::string::npos) {
            return LoadError{"config: stt.base_url: must include scheme (http://...)"};
        }
        if (cfg.stt.model.empty()) {
            return LoadError{"config: stt.model: required when stt.base_url is set"};
        }
        if (cfg.stt.request_timeout_seconds == 0) {
            return LoadError{"config: stt.request_timeout_seconds: must be > 0"};
        }
    }
    // TTS goes through Speaches' OpenAI-API-compatible endpoint. Every
    // configured voice resolves through the registry to a non-empty
    // {model_id, voice_id} tuple (Speaches returns HTTP 422 when the
    // request is missing the `voice` field — catch it at config load).
    if (!cfg.tts.voices.empty() && cfg.tts.base_url.empty()) {
        return LoadError{"config: tts.base_url: must be set when tts.voices is non-empty"};
    }
    if (!cfg.tts.base_url.empty()
        && cfg.tts.base_url.find("://") == std::string::npos) {
        return LoadError{"config: tts.base_url: must include scheme (http://...)"};
    }
    for (const auto& [lang, alias] : cfg.tts.voices) {
        if (alias.empty()) {
            return LoadError{"config: tts.voices[" + lang
                + "]: alias name required (e.g. en-amy, ru-irina)"};
        }
        auto rit = cfg.tts.voices_resolved.find(lang);
        if (rit == cfg.tts.voices_resolved.end()) {
            return LoadError{"config: tts.voices[" + lang + "]: unknown alias '"
                + alias + "' — register it under models.tts or pick a "
                + "configured one"};
        }
        if (rit->second.model_id.empty() || rit->second.voice_id.empty()) {
            return LoadError{"config: models.tts[" + alias
                + "]: id + voice required"};
        }
    }
    // If tts.voices is non-empty, the fallback_lang must point to one
    // of them — otherwise PiperClient would have no route at all when
    // the detected language is missing from the map.
    if (!cfg.tts.voices.empty()
        && !cfg.tts.voices.contains(cfg.tts.fallback_lang)) {
        return LoadError{
            "config: tts.fallback_lang ('" + cfg.tts.fallback_lang
            + "') must match one of tts.voices keys"};
    }
    if (cfg.audio.sample_rate_hz == 0) {
        return LoadError{"config: audio.sample_rate_hz: must be > 0"};
    }
    if (cfg.audio.buffer_frames == 0) {
        return LoadError{"config: audio.buffer_frames: must be > 0"};
    }
    // playback.max_queue_chunks: 0 means unbounded (pre-M7 default).
    // No upper bound to validate against — memory is the limit.
    if (cfg.dialogue.max_tts_queue_sentences == 0) {
        return LoadError{"config: dialogue.max_tts_queue_sentences: must be > 0"};
    }

    // M4 — VAD / utterance.
    if (cfg.vad.onset_threshold < 0.0F || cfg.vad.onset_threshold > 1.0F) {
        return LoadError{"config: vad.onset_threshold: must be in [0, 1]"};
    }
    if (cfg.vad.offset_threshold < 0.0F || cfg.vad.offset_threshold > 1.0F) {
        return LoadError{"config: vad.offset_threshold: must be in [0, 1]"};
    }
    if (cfg.vad.offset_threshold > cfg.vad.onset_threshold) {
        return LoadError{
            "config: vad.offset_threshold must be <= vad.onset_threshold "
            "(otherwise the hysteresis band collapses)"};
    }
    if (cfg.vad.min_speech_ms == 0) {
        return LoadError{"config: vad.min_speech_ms: must be > 0"};
    }
    if (cfg.vad.hangover_ms == 0) {
        return LoadError{"config: vad.hangover_ms: must be > 0"};
    }
    if (cfg.utterance.max_in_flight == 0) {
        return LoadError{"config: utterance.max_in_flight: must be > 0"};
    }
    if (cfg.utterance.max_duration_ms == 0) {
        return LoadError{"config: utterance.max_duration_ms: must be > 0"};
    }

    // M6 — APM / loopback.
    if (cfg.audio.loopback.ring_seconds == 0) {
        return LoadError{"config: audio.loopback.ring_seconds: must be > 0"};
    }
    if (cfg.apm.max_delay_ms == 0) {
        return LoadError{"config: apm.max_delay_ms: must be > 0"};
    }
    if (cfg.apm.initial_delay_estimate_ms > cfg.apm.max_delay_ms) {
        return LoadError{
            "config: apm.initial_delay_estimate_ms must be <= apm.max_delay_ms"};
    }

    // M7 — barge-in.
    if (cfg.barge_in.min_aec_erle_db < 0.0F) {
        return LoadError{"config: barge_in.min_aec_erle_db: must be >= 0"};
    }
    if (cfg.barge_in.min_vad_probability < 0.0F
        || cfg.barge_in.min_vad_probability > 1.0F) {
        return LoadError{"config: barge_in.min_vad_probability: must be in [0, 1]"};
    }
    return std::nullopt;
}

// Walk the catalog in `cfg.models` and rewrite the subsystem fields
// that name a model. Idempotent; runs once at load time before
// validate(). Anything not registered passes through unchanged
// (back-compat for configs that put the full id directly), EXCEPT
// `tts.voices` — every entry there must resolve through the registry.
// See ModelsConfig in config.hpp for the full design rationale.
namespace {
void resolve_aliases(Config& cfg) {
    // ---- LLM: alias → label override (metadata-only until M8A). ----
    if (auto it = cfg.models.llm.find(cfg.llm.model);
        it != cfg.models.llm.end()) {
        // We deliberately keep `cfg.llm.model` set to the alias name —
        // the OpenAI-endpoint label that llama-server reports via
        // /v1/models. The resolved filename is in `it->second.file`
        // for now; M8A will thread it into a model-controller call.
    }
    // ---- STT: alias → full HF id. ----
    if (auto it = cfg.models.stt.find(cfg.stt.model);
        it != cfg.models.stt.end()) {
        cfg.stt.model = it->second.id;
    }
    // ---- VAD: alias → relative path under the XDG data root. ----
    // Registry stores bare filenames; the resolver prepends the
    // canonical per-type subdir (`models/silero/`) so the existing
    // bootstrap.cpp `resolve_data_path` call lands on
    // ${XDG}/acva/models/silero/<file>. tools/acva-models places the
    // file in the same location, so the mapping is symmetric.
    if (auto it = cfg.models.vad.find(cfg.vad.model_path);
        it != cfg.models.vad.end()) {
        cfg.vad.model_path = "models/silero/" + it->second.file;
    }
    // ---- TTS: alias → {model_id, voice_id}. ----
    cfg.tts.voices_resolved.clear();
    for (const auto& [lang, alias] : cfg.tts.voices) {
        auto it = cfg.models.tts.find(alias);
        if (it == cfg.models.tts.end()) continue;        // validate() reports
        cfg.tts.voices_resolved[lang] =
            TtsVoice{ .model_id = it->second.id,
                       .voice_id = it->second.voice };
    }
}
} // namespace

LoadResult load_from_string(std::string_view yaml) {
    Config cfg;
    auto ec = glz::read_yaml(cfg, yaml);
    if (ec) {
        return LoadError{"config: parse error: " + glz::format_error(ec, yaml)};
    }
    resolve_aliases(cfg);
    if (auto verr = validate(cfg)) {
        return *verr;
    }
    return cfg;
}

LoadResult load_from_file(const std::filesystem::path& path) {
    LoadError err;
    auto text = read_file(path, err);
    if (!err.message.empty()) {
        return err;
    }
    return load_from_string(text);
}

} // namespace acva::config
