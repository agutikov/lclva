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
    dir,
};

struct LoggingConfig {
    std::string level = "info"; // trace | debug | info | warn | error | critical | off
    // Default `stderr` keeps minimal/test configs valid (no extra
    // path required). Production uses `dir` — see config/default.yaml.
    std::string sink = "stderr"; // "stderr" | "journal" | "file" | "dir"
    std::optional<std::string> file_path;
    // M6 — per-run log directory. When `sink: dir`, log::init writes
    // to `{dir_path}/acva-YYYYMMDD-HHMMSS.log` (local time at startup;
    // matches the timestamp format in log lines). The directory is
    // created on first run; on permission failure the logger falls
    // back to stderr with a warning printed first. Each `acva`
    // invocation gets its own file so soak runs / debug sessions
    // are easy to triage post-mortem without log rotation.
    //
    // Default `/var/log/acva` requires the dir to be pre-created
    // with user-writable permissions:
    //   sudo install -d -o $USER -g $USER -m 0755 /var/log/acva
    // If you can't do that, set this to e.g. `~/.local/state/acva/logs`.
    std::optional<std::string> dir_path;
    // M6 — tee every log line to stderr in addition to the primary
    // sink. Useful when sink=dir (or sink=file) and you want to
    // watch the run live in the terminal AND keep the per-run log
    // for post-mortem. No effect when sink is already stderr.
    bool mirror_to_stderr = false;
    // M6 — periodic GPU VRAM probe. When > 0, a background thread
    // shells `nvidia-smi --query-gpu=memory.used,memory.free` every
    // N ms and emits a structured `vram` log event ONLY on transitions
    // across `vram_low_threshold_mib` (edge-triggered). This catches
    // the creeping-OOM pattern where Speaches' encoder workspace pushes
    // free VRAM toward zero before a request finally fails, without
    // spamming the log under healthy conditions. Set interval to 0 to
    // disable; default 1000 ms is light (one nvidia-smi exec / sec).
    // Quietly no-ops if nvidia-smi is missing.
    uint32_t vram_monitor_interval_ms = 1000;
    // Free-VRAM (MiB) below which the monitor emits a `vram_low` event.
    // When VRAM recovers above the threshold, emits `vram_recovered`.
    // 100 MiB picks up the M6B faster-whisper #992 leak symptom (see
    // open_questions.md L7) before inference allocations OOM.
    uint32_t vram_low_threshold_mib = 100;
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
    // Nucleus sampling — keep tokens whose cumulative prob mass is <= top_p.
    // 1.0 disables truncation (the LLM full vocabulary). Personalities lean on
    // this to widen ("bender", "ingenue") or tighten ("consultant") sampling.
    double top_p = 1.0;
    // Repetition penalty (llama.cpp-native parameter; OpenAI's
    // `frequency_penalty` is a separate, additive knob and we don't ship that
    // here). 1.0 = no penalty. Higher (1.1–1.2) suppresses near-repeats —
    // helpful for chatty personalities so they don't loop verbal tics.
    double repeat_penalty = 1.0;
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
    // M6 — force a tiny synthetic transcription at acva startup to
    // pre-load the Whisper model into VRAM. Without this, the first
    // user turn pays the model-load cost (~3 s for medium /
    // large-v3 / int8_float16 quantised models). Synchronous: blocks
    // pipeline-open until the warm-up returns. Default true; flip
    // off for fast-startup tests or when running against a remote
    // Speaches that's already warm.
    bool warmup_on_startup = true;
    // M7 Bug 4 — minimum int16 RMS for an utterance slice to be sent
    // to STT. Whisper is prone to projecting subtitle hallucinations
    // ("Продолжение следует…", "Субтитры сделал DimaTorzok") onto
    // near-silent audio that crossed VAD's onset_threshold thanks to
    // pre/post padding. Slices with RMS below this are dropped before
    // UtteranceReady fires; the M4B path then never POSTs the silent
    // buffer. 0 disables the gate (pre-M7 behaviour). 200 = ~ -45 dBFS,
    // a safe floor for genuine close-mic speech without clipping
    // soft-spoken users.
    std::uint32_t min_utterance_rms = 200;
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
    // YAML schema: BCP-47 language code → registry alias name from
    // `models.tts`. Each value is a short name (e.g. "en-amy",
    // "ru-irina"); the alias is resolved to a TtsVoice {model_id,
    // voice_id} at config-load time and stashed in `voices_resolved`
    // below. Downstream code reads `voices_resolved`, not `voices`.
    std::map<std::string, std::string> voices;
    // Filled by config::resolve_aliases() from `voices` + `models.tts`.
    // NOT parsed from YAML — Glaze leaves it default-constructed during
    // `read_yaml` because the YAML key for this struct (`tts.voices`)
    // is mapped to the alias-string field above. All callers that need
    // a TtsVoice tuple read this map. `tts.voices` (alias names) stays
    // around as a debug/round-trip aid.
    std::map<std::string, TtsVoice> voices_resolved;
    // Used when the detected language has no entry in `voices`.
    std::string fallback_lang = "en";
    uint32_t request_timeout_seconds = 10;
    // Spoken tempo in words-per-minute. Translated to the OpenAI
    // `speed` request field at native_wpm = 160 baseline (typical
    // Piper voice). 200 wpm ≈ speed=1.25 — a touch faster than
    // conversational, comfortable for an assistant. Validated to
    // [50, 400] which maps to speed [0.31, 2.5]. Set to 160 for
    // unmodified native cadence; 0 also disables the field.
    uint32_t tempo_wpm = 200;
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

// M6 — self-listen feedback loop. When enabled, the bridge
// accumulates a copy of each synthesized sentence's PCM and the
// orchestrator pushes it through STT in a background thread, then
// logs the original LLM text alongside the transcribed audio. Catches
// the class of bugs where the LLM produced correct text but the
// listener heard something different (queue overflow drops, voice
// dropouts, mid-stream truncation, mispronounced characters).
struct SelfListenConfig {
    bool enabled = false;
    // Cap concurrent in-flight STT calls. Self-listen on top of
    // production STT can starve VRAM on small cards, so we throttle.
    uint32_t max_in_flight = 1;
};

struct DialogueConfig {
    uint32_t recent_turns_n = 10;
    uint32_t max_assistant_sentences = 6;
    uint32_t max_assistant_tokens = 400;
    SelfListenConfig self_listen;
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
    // M6 — when true (default on Linux), `acva` writes a minimal
    // ALSA config to a tmpfile and points `ALSA_CONFIG_PATH` at it
    // BEFORE any `Pa_Initialize` runs. This stops PortAudio's ALSA
    // host-API enumeration from probing every PCM listed in the
    // system asound.conf — most importantly the synthetic `jack`
    // PCM that pipewire-jack exposes, whose snd_pcm_close call
    // deadlocks pipewire's thread-loop on shutdown and adds ~4 min
    // to startup time. With this flag on, only `default` is
    // probed, routing through PulseAudio (pipewire-pulse on
    // PipeWire systems). Set false to use the system asound.conf
    // verbatim — required if you genuinely need a non-pulse ALSA
    // route or have customised your asound.conf in load-bearing
    // ways.
    bool skip_alsa_full_probe = true;
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
    // M6B — when true, ensure PipeWire's `module-echo-cancel` is loaded
    // on startup and route this process's audio through it (via
    // PULSE_SINK / PULSE_SOURCE env vars). Recommended on Linux desktops
    // where PipeWire is the audio server and the in-process WebRTC APM
    // doesn't cancel well on the integrated codec — see
    // docs/aec_report.md § 6 + plans/milestones/m6b_aec_hardware.md
    // Step 3. When this is on, set `aec_enabled: false` so the
    // in-process AEC doesn't try to subtract the echo a second time
    // (it has nothing to cancel and the convergence transient produces
    // ~800 ms of zeroed output at startup). NS and AGC may stay on.
    bool use_system_aec = false;
};

struct PlaybackConfig {
    // Cap on chunks queued ahead of the audio callback. 0 means
    // **unbounded** — pre-M7 default, since the LLM's response is
    // finite (bounded by max_assistant_tokens) and we'd rather hold
    // the full monologue in RAM than drop chunks mid-sentence. At
    // ~50 ms per chunk and the typical max-tokens response (~4 min,
    // ~5000 chunks, ~25 MB), the unbounded queue tops out well below
    // any realistic memory budget. Once M7 (barge-in) lands we'll
    // re-introduce a bound so a runaway LLM can't pile audio on
    // forever — the cancellation path then drains the queue cleanly.
    uint32_t max_queue_chunks = 0;
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

// M7 — barge-in detector. Promotes a VAD `SpeechStarted` during
// `Speaking` into a `UserInterrupted` event, but only after AEC has
// converged enough to ignore the assistant's own voice. See
// plans/milestones/m7_barge_in.md §1.
struct BargeInConfig {
    // Master switch. When false the detector is not constructed and
    // M0's pre-barge-in behaviour stands (FSM has no detection layer
    // upstream of UserInterrupted; only synthetic / programmatic
    // sources publish it).
    bool enabled = true;
    // When true (default), the detector refuses to fire unless
    // `Apm::aec_active()` is true AND `erle_db()` >= min_aec_erle_db.
    // Set to false on headphone-only setups (no echo path) where AEC
    // never converges because there's nothing to cancel — the loopback
    // sink's `aec_active` may still be false in that mode.
    bool require_aec_converged = true;
    // ERLE gate. The M6 acceptance fixture targets > 25 dB; pre-M6B
    // hardware that uses the in-process APM might only reach 15-20 dB
    // briefly, so the threshold is set just above the noise floor.
    float min_aec_erle_db = 15.0F;
    // Lower bound on Silero VAD probability at the moment the detector
    // accepts a SpeechStarted. Tighter than the regular endpointer's
    // onset_threshold so transient speaker artifacts that slip past
    // AEC don't fire barge-in. Currently advisory: the M4 endpointer
    // already enforces its own threshold before publishing
    // SpeechStarted; this value is reserved for a future change that
    // exposes the per-frame probability through a side channel.
    float min_vad_probability = 0.6F;
    // Quiet window after Speaking begins during which barge-in is
    // suppressed. Absorbs the user's residual breath / lip sound at
    // the moment TTS starts, plus any first-buffer convergence
    // transient in the AEC.
    uint32_t cool_down_after_turn_ms = 300;
    // FinalTranscripts whose normalised text is shorter than this are
    // treated as discarded utterances and not sent to the LLM. Catches
    // brief coughs / throat-clearing that crossed the VAD threshold
    // (often after a barge-in) and would otherwise cost an LLM call.
    // Counted in code points after trimming whitespace + control
    // characters.
    uint32_t min_real_utterance_chars = 3;
};

// Model registry — short alias → backend-specific resource locator.
// One catalog section per model type. `purpose` is a short
// human-readable note (used in /status and `acva models list` once
// M8A's admin sidebar lands). The aliases mirror the names used in
// `tools/acva-models` so a user who installs a model via the
// downloader can refer to it by the same name in config.
//
// Resolution rule: for each subsystem field that names a model, the
// loader checks the matching catalog. If the string is a registered
// alias, it's replaced with the alias's resource locator (filename
// for LLM/VAD, HF id for STT, full {model_id, voice_id} tuple for
// TTS). If not, the string is left as-is — back-compat with configs
// that put the full id directly. TTS is the exception: voice values
// MUST be aliases (the alternative full-tuple form was removed when
// the registry landed; aliasing was specifically introduced to make
// per-language TTS picks one-line edits instead of two-field
// objects).
//
// LLM aliasing is currently metadata-only: `cfg.llm.model` is just
// the OpenAI-endpoint label, and the actual GGUF llama-server loads
// is set by ACVA_LLM_MODEL in packaging/compose/.env. M8A wires the
// resolved filename into a model-controller sidecar so the alias
// becomes load-bearing — see `plans/milestones/m8a_admin_state.md`.

struct LlmModelEntry {
    // GGUF filename inside ${ACVA_MODELS_DIR}/llama.cpp/. Becomes
    // load-bearing in M8A; informational today.
    std::string file;
    // Direct-download URL used by `tools/acva-models install <alias>`.
    // Optional: aliases without `url` are read-only references the
    // user must place on disk by hand.
    std::string url;
    // Expected file size in bytes. Optional; when set, the downloader
    // verifies post-fetch and resumes partial downloads from this
    // anchor. 0 disables the check.
    std::uint64_t size = 0;
    // One-line description shown by `acva-models list` and (M8A)
    // `acva models list`.
    std::string purpose;
};

struct SttModelEntry {
    // Full HuggingFace model id passed to Speaches' /v1/models/{id}
    // download endpoint and `cfg.stt.model` after resolution.
    // Speaches owns the actual file fetch — no `url` needed.
    std::string id;
    std::string purpose;
};

struct TtsModelEntry {
    // Full HuggingFace model id of the Speaches TTS model.
    std::string id;
    // Speaker id within the model. For single-speaker Piper voices,
    // typically the speaker name (e.g. "amy"). For multi-speaker
    // models like Kokoro, the published voice id (e.g. "af_bella").
    // Same model id may appear in multiple aliases that differ only
    // in `voice` — that's the Kokoro shape.
    std::string voice;
    std::string purpose;
};

struct VadModelEntry {
    // Filename inside ${ACVA_MODELS_DIR}/silero/.
    std::string file;
    // Direct-download URL used by `tools/acva-models install <alias>`.
    std::string url;
    // Expected file size in bytes. Optional; 0 disables the check.
    std::uint64_t size = 0;
    std::string purpose;
};

struct ModelsConfig {
    std::map<std::string, LlmModelEntry> llm;
    std::map<std::string, SttModelEntry> stt;
    std::map<std::string, TtsModelEntry> tts;
    std::map<std::string, VadModelEntry> vad;
};

// Personality registry — a thin overlay on top of the existing subsystem
// configs. A Personality bundles a system prompt, a voice, a TTS tempo,
// LLM sampling shape, and response-length caps into one named preset
// (e.g. `consultant`, `bender`). Switching personalities is a one-line
// edit to `active_personality` plus a restart — no other section changes.
//
// Resolution: when `active_personality` is non-empty,
// `config::resolve_aliases()` overlays each present field of the named
// personality onto the corresponding top-level field BEFORE the existing
// alias-resolution step. Missing fields keep the top-level default; for
// the per-language maps (`system_prompts`, `voices`) only the supplied
// languages are overlaid — others fall back to whatever's in the
// top-level map. That means the top-level fields stay the "neutral
// baseline" and a personality is just the diff.
//
// All scalar fields are `std::optional` so glaze treats them as
// "absent unless set". An unknown `active_personality` is a hard
// validation error — silently ignoring would let a typo erase the
// user's intended persona.

// Per-personality LLM sampling overrides. Each field, if present,
// replaces the corresponding `LlmConfig` field at config-load.
struct PersonalityLlmOverride {
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<double> repeat_penalty;
};

// Per-personality dialogue/length-cap overrides.
struct PersonalityDialogueOverride {
    std::optional<uint32_t> max_assistant_sentences;
    std::optional<uint32_t> max_assistant_tokens;
    // Folds into cfg.dialogue.sentence_splitter.max_sentence_chars.
    // Most personalities don't need to touch this, but rambling
    // philosophers benefit from longer chunks (less mid-thought TTS
    // boundaries) and terse personas benefit from shorter ones.
    std::optional<uint32_t> max_sentence_chars;
};

struct Personality {
    // Free-form one-liner shown by /status (post-M8A) and useful as a
    // YAML anchor when the user is browsing the registry. Not consumed
    // by the runtime.
    std::string description;
    // BCP-47 lang code → system-prompt text. Partial maps are fine;
    // missing langs fall back to the top-level
    // `dialogue.system_prompts[lang]`.
    std::map<std::string, std::string> system_prompts;
    // BCP-47 lang code → registry alias from `models.tts`. Partial
    // maps are fine; missing langs fall back to the top-level
    // `tts.voices[lang]`. The personality voice is treated like a
    // film dub: same character, localized voicing.
    std::map<std::string, std::string> voices;
    // Per-personality TTS tempo. Frivolous = fast, philosopher = slow.
    std::optional<uint32_t> tempo_wpm;
    PersonalityLlmOverride       llm;
    PersonalityDialogueOverride  dialogue;
};

struct Config {
    ModelsConfig models;
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
    BargeInConfig barge_in;
    // Personality registry + active selection. Empty `active_personality`
    // means "no overlay" — the top-level fields are used verbatim. See
    // `Personality` above for the overlay semantics.
    std::map<std::string, Personality> personalities;
    std::string active_personality;
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
