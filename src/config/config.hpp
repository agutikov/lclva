#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace acva::config {

enum class LogSink {
    stderr_,
    journal,
    file,
};

struct LoggingConfig {
    std::string level = "info"; // trace | debug | info | warn | error | critical | off
    std::string sink = "stderr"; // "stderr" | "journal" | "file"
    std::optional<std::string> file_path;
    // When true, main.cpp installs a bus subscriber that emits a
    // structured log line for each transcript / LLM / TTS / playback
    // event with the relevant payload (transcript text, sentence
    // text, etc). Useful for end-to-end debugging; rate-limited
    // events (LlmToken, TtsAudioChunk) are summarized rather than
    // logged per-occurrence. Default on for the M5 default-config
    // path that drives the full speech-to-speech pipeline.
    bool trace_events = true;
};

struct ControlConfig {
    std::string bind = "127.0.0.1";
    uint16_t port = 9876;
};

struct PipelineConfig {
    // Drive the FSM with synthetic events (no real audio/STT/LLM/TTS).
    // Default off — useful only as an M0-era demo. Set to true in YAML
    // to exercise the FSM end-to-end without configuring backends.
    bool fake_driver_enabled = false;
    // Sentences per synthetic turn.
    uint32_t fake_sentences_per_turn = 3;
    // Idle between synthetic turns, in ms.
    uint32_t fake_idle_between_turns_ms = 1500;
    // Barge-in probability per turn (0.0 .. 1.0).
    double fake_barge_in_probability = 0.0;
};

// Per-service health probe knobs. Embedded in each backend's config
// section (llm/stt/tts) so the user can tune services independently. The
// Supervisor (M2) reads these to drive ServiceMonitor.
struct ServiceHealthConfig {
    // Empty disables this service's probe entirely (the Supervisor still
    // registers it with state=NotConfigured for /status visibility).
    std::string health_url;
    // If true and the service stays Unhealthy past the supervisor's grace
    // window, the dialogue path is gated and refuses new turns.
    bool fail_pipeline_if_down = true;
    uint32_t probe_interval_healthy_ms = 5000;
    uint32_t probe_interval_degraded_ms = 1000;
    // Consecutive probe failures from Healthy before transitioning to
    // Unhealthy. Failures within [1, this) sit in Degraded.
    uint32_t degraded_max_failures = 3;
};

struct LlmConfig {
    std::string base_url = "http://127.0.0.1:8081/v1";
    std::string model = "qwen2.5-7b-instruct";
    std::string unit = "acva-llama.service";
    double temperature = 0.7;
    uint32_t max_tokens = 400;
    uint32_t max_prompt_tokens = 3000;
    uint32_t request_timeout_seconds = 60;
    uint32_t keep_alive_interval_seconds = 60;
    ServiceHealthConfig health;
};

struct SttConfig {
    // M4B: OpenAI-API-compatible base URL. Empty disables real STT
    // (the orchestrator falls back to whatever drives FinalTranscript
    // events — e.g. the synthetic FakeDriver). When set, OpenAiSttClient
    // POSTs each UtteranceReady's audio to {base_url}/audio/transcriptions
    // and publishes the resulting FinalTranscript on the bus.
    std::string base_url;
    // Speaches model id for transcription, e.g.
    // "Systran/faster-whisper-large-v3". Required when base_url is set.
    std::string model;
    // Per-request timeout for the transcription POST.
    uint32_t request_timeout_seconds = 30;
    // M5 — BCP-47 language code passed into Speaches'
    // `input_audio_transcription.language` (configured, NOT detected;
    // Speaches' realtime endpoint doesn't return per-utterance
    // detected language). Also stamped onto `FinalTranscript.lang`
    // by RealtimeSttClient so downstream consumers
    // (PromptBuilder system-prompt selection, TTS voice routing,
    // memory `lang` column) get a non-empty value. Default "en".
    // M9 (post-MVP) replaces this with per-utterance detection.
    std::string language = "en";
    // M5 — when true and base_url is set, the orchestrator uses
    // RealtimeSttClient (WebRTC streaming on /v1/realtime) and wires
    // it to the M4 capture pipeline's live-audio sink. When false,
    // the M4B OpenAiSttClient (request/response on
    // /v1/audio/transcriptions) is used instead — useful for fixture
    // demos and when libdatachannel isn't available. Default true so
    // a normal `acva` run uses streaming.
    bool streaming = true;
    ServiceHealthConfig health;
};

// One Piper service per language. Upstream `piper.http_server` only
// loads one voice per process, so each language gets its own port and
// container. The PiperClient picks the URL by detected language.
// Per-language TTS voice descriptor for the Speaches OpenAI-API-compatible
// TTS endpoint. `model_id` selects the Speaches model
// (e.g. "speaches-ai/piper-en_US-amy-medium"); `voice_id` is the speaker
// within the model — required by the API even for single-speaker
// Piper voices (typically the speaker name, e.g. "amy"; Kokoro models
// use ids like "af_bella").
struct TtsVoice {
    std::string model_id;
    std::string voice_id;
};

struct TtsConfig {
    ServiceHealthConfig health;
    // OpenAI-API-compatible base URL for `POST /v1/audio/speech`.
    // Required when `voices` is non-empty.
    std::string base_url;
    // Map of BCP-47 language code → voice descriptor.
    std::map<std::string, TtsVoice> voices;
    // Used when the detected language has no entry in `voices`.
    std::string fallback_lang = "en";
    uint32_t request_timeout_seconds = 10;
};

struct SupervisorConfig {
    // How long the LLM may stay Unhealthy before pipeline_state flips to
    // failed. Counts from the Unhealthy transition, not from the first
    // failed probe.
    uint32_t pipeline_fail_grace_seconds = 30;
    // Per-probe HTTP timeout. Applied uniformly across services.
    uint32_t probe_timeout_ms = 3000;
};

struct MemorySummaryConfig {
    std::string trigger = "turns";    // turns | tokens | idle | hybrid
    uint32_t turn_threshold = 15;
    uint32_t token_threshold = 4000;
    uint32_t idle_seconds = 120;
    std::string language = "dominant"; // dominant | english | recent
};

struct MemoryFactsConfig {
    std::string policy = "conservative"; // conservative | moderate | aggressive | manual_only
    double confidence_threshold = 0.7;
};

struct MemoryConfig {
    // Empty → resolved to ${XDG_DATA_HOME:-$HOME/.local/share}/acva/acva.db
    // at startup via config::resolve_data_path. Relative paths are
    // resolved against the same XDG root. Absolute paths are used as-is.
    // The resolver `mkdir -p`s the parent directory for first-run UX.
    std::string db_path;
    uint32_t recent_turns_n = 10;
    uint32_t write_queue_capacity = 256;
    MemorySummaryConfig summary;
    MemoryFactsConfig facts;
};

struct SentenceSplitterConfig {
    uint32_t max_sentence_chars = 600; // forced flush threshold
    bool detect_code_fences = true;
};

struct DialogueConfig {
    uint32_t recent_turns_n = 10;
    uint32_t max_assistant_sentences = 6;
    uint32_t max_assistant_tokens = 400;
    // Backpressure on the TTS path: when the playback queue depth
    // crosses this, the dialogue layer pauses pulling more sentences
    // off the LLM stream. Independent of max_assistant_sentences.
    uint32_t max_tts_queue_sentences = 3;
    std::string fallback_language = "en";
    // Map of lang code → system-prompt text. English is required.
    std::map<std::string, std::string> system_prompts;
    SentenceSplitterConfig sentence_splitter;
};

// M6 — loopback ring sizing. The PlaybackEngine taps the audio buffer
// it just emitted into a ring of this duration, indexed by emit
// timestamp. The APM wrapper pulls aligned reference frames from it.
// 2 s easily covers the typical speaker→mic round-trip (50 ms physical
// + a few hundred ms of pipeline jitter under load).
struct AudioLoopbackConfig {
    uint32_t ring_seconds = 2;
};

struct AudioConfig {
    // PortAudio device selector. "default" → host default; otherwise
    // matched by name substring (case-insensitive). Empty == "default".
    std::string output_device = "default";
    // M4 — capture device. "default" picks the host default; "none"
    // disables capture entirely. Same matching rules as output.
    std::string input_device = "default";
    // Master flag for the M4 capture path. When false, the
    // CaptureEngine + AudioPipeline are not constructed and the fake
    // driver continues to publish synthetic Speech* events when
    // enabled.
    bool capture_enabled = false;
    uint32_t sample_rate_hz = 48000;
    // Frames-per-callback for the PortAudio output stream. 480 frames
    // = 10 ms at 48 kHz. Larger = more headroom against underruns;
    // smaller = lower playback start latency.
    uint32_t buffer_frames = 480;
    // M5 — half-duplex fallback for the speakers-without-AEC path.
    // When true and `capture_enabled` is also true, the CaptureEngine
    // drops mic samples while the dialogue FSM is in Speaking state
    // (and for `half_duplex_hangover_ms` after it leaves), preventing
    // the assistant's own voice from triggering VAD. Trade-off: no
    // barge-in. M6 AEC + M7 barge-in supersede this once landed.
    bool     half_duplex_while_speaking = false;
    uint32_t half_duplex_hangover_ms    = 200;
    // M6 — AEC reference-signal loopback ring.
    AudioLoopbackConfig loopback;
};

// M6 — WebRTC AudioProcessing (AEC + NS + AGC) tuning. Mirrors
// `audio::ApmConfig` field-for-field; main.cpp maps one to the other
// when constructing the engine. Defaults match the M6 plan §6.
struct ApmConfig {
    bool aec_enabled = true;
    bool ns_enabled  = true;
    bool agc_enabled = true;
    // Initial guess for `set_stream_delay_ms`. APM's internal estimator
    // refines from here over the first ~3 seconds.
    uint32_t initial_delay_estimate_ms = 50;
    // Sanity ceiling on the estimated delay; over this we log + clamp.
    uint32_t max_delay_ms = 250;
};

struct PlaybackConfig {
    // Hard cap on chunks queued ahead of the audio callback. At ~10 ms
    // per chunk this is ~640 ms of audio.
    uint32_t max_queue_chunks = 64;
    // Throttle window for the "underrun" log line so a stuck pipeline
    // doesn't spam the logs.
    uint32_t underrun_log_throttle_ms = 1000;
    // Per-turn pre-buffer: don't start consuming from the queue for a
    // new active turn until at least this many milliseconds of audio
    // have queued up. Absorbs the libcurl-receive jitter pattern of
    // streaming TTS — Speaches sends bursts faster than realtime, but
    // not perfectly evenly, so the audio thread would otherwise
    // alternate between draining-fast and underrunning. 0 disables
    // prebuffering (legacy M3 behaviour). 100 ms is the M4B default;
    // tune up to ~250 ms if a slow TTS pipeline still produces
    // underruns. Counts only samples for the *active* turn — barge-in
    // or speculation-cancel resets the prefill state.
    uint32_t prefill_ms = 100;
};

// M4 — Silero VAD knobs. `provider` is currently a label (silero is
// the only option); reserved for future per-provider routing.
struct VadConfig {
    std::string provider = "silero";
    // Path to the ONNX model file. Empty disables the VAD; the audio
    // pipeline still publishes Speech* events but the endpointer will
    // never fire on real speech (probability is fixed at 0).
    std::string model_path;
    float onset_threshold  = 0.5F;
    float offset_threshold = 0.35F;
    uint32_t min_speech_ms  = 200;
    uint32_t hangover_ms    = 600;
    uint32_t pre_padding_ms = 300;
    uint32_t post_padding_ms = 100;
};

struct UtteranceConfig {
    uint32_t max_in_flight    = 3;
    uint32_t max_duration_ms  = 60000; // safety cap for long monologues
};

struct Config {
    LoggingConfig logging;
    ControlConfig control;
    PipelineConfig pipeline;
    LlmConfig llm;
    SttConfig stt;
    TtsConfig tts;
    SupervisorConfig supervisor;
    MemoryConfig memory;
    DialogueConfig dialogue;
    AudioConfig audio;
    PlaybackConfig playback;
    VadConfig vad;
    UtteranceConfig utterance;
    ApmConfig apm;
};

struct LoadError {
    std::string message;
};

// LoadResult is std::variant<Config, LoadError>. Use std::holds_alternative or
// std::get_if to discriminate. C++20 doesn't have std::expected; this is a
// stable substitute.
using LoadResult = std::variant<Config, LoadError>;

[[nodiscard]] LoadResult load_from_file(const std::filesystem::path& path);
[[nodiscard]] LoadResult load_from_string(std::string_view yaml);

[[nodiscard]] std::optional<LoadError> validate(const Config& cfg);

[[nodiscard]] LogSink parse_log_sink(std::string_view s);

} // namespace acva::config
