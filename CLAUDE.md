# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

**acva** — Autonomous Conversational Voice Agent. A local, production-grade C++ voice assistant for multi-hour conversations on a single workstation (RTX 4060). All inference is local; model backends (llama.cpp, whisper.cpp, Piper) run as **Docker Compose containers** in dev (with systemd as an alternative production path); the C++ orchestrator runs as a host CLI binary and owns control plane, audio I/O, state, memory, and observability.

Target hardware: Linux x86_64, RTX 4060 8 GB. Speakers + AEC is the primary UX (not headphones).
Multilingual STT/TTS/LLM with auto language detection per turn.
Streaming partial STT with speculative LLM start (M5 — three options on the table; see `plans/open_questions.md` L1).

## Status

**M0 + M1 + M2 + M3 + M4 + M4B + M5 + M6 (steps 1–6) complete.** Test suite split into `acva_unit_tests` (260 cases, no external deps) and `acva_integration_tests` (13 cases, real Silero model + live Speaches). Both pass green on the dev workstation without any env vars; integration tests resolve assets via the XDG defaults main.cpp uses. **Compose stack is `llama` + `speaches` only** — Speaches replaces standalone `whisper.cpp/server` (STT) and `piper.http_server` (TTS) behind one OpenAI-API-compatible surface. TTS goes through `OpenAiTtsClient` (libcurl streaming PCM); STT through `OpenAiSttClient` (multipart `POST /v1/audio/transcriptions`, blocking, request/response — M5 swaps for streaming/realtime). `PlaybackEngine` carries a per-turn pre-buffer threshold (`cfg.playback.prefill_ms`, default 100 ms) that absorbs streaming-TTS chunk-arrival jitter — measured 56–71% fewer underruns without any total-latency regression. Demos `acva demo {tts,chat,stt}` exercise the new wiring; `acva demo stt` is a self-contained TTS-fixture-audio → STT round-trip. STT model is `Systran/faster-whisper-large-v3` running with `WHISPER__COMPUTE_TYPE=int8_float16` (set in compose env); fits cleanly when llama is stopped or running smaller. With the default llama-7B-Q4 still loaded, fall back to `Systran/faster-whisper-medium` — see memory note `project_gpu_cdi_and_vram.md` for the 8 GB VRAM budget rationale and the test-vs-config alignment trap. **Next: M7 — barge-in.** M6 Step 7 (real-hardware VAD re-baseline +
the > 25 dB ERLE acceptance gate) waits on a measurement session
with the dev workstation's actual speakers + mic; the synthetic
demo (`acva demo aec`) gives ~22 dB mic-energy reduction on a 1 kHz
fixture and confirms the loopback → APM → cleaned-mic wiring is
live.

**M6 (AEC):** PlaybackEngine now taps the chunk it just emitted into
an `audio::LoopbackSink` ring (sized by `cfg.audio.loopback.ring_seconds`,
default 2 s). The capture pipeline inserts an APM stage between
resample and VAD: `SPSC ring → Resample → APM → VAD → Endpointer`.
APM (`audio::Apm`) wraps `webrtc::AudioProcessing` from the system
package `webrtc-audio-processing-1` 1.3 (BSD-3, Arch `extra`); the
build gates on `ACVA_HAVE_WEBRTC_APM` and falls back to a
pass-through stub when missing. `voice_aec_delay_estimate_ms`,
`voice_aec_erle_db`, and `voice_aec_frames_processed_total` join
`/metrics`; `/status` gains an `apm` block. The compose stack reaches all-three-healthy on the dev workstation; `acva --stdin` drives a real LLM end-to-end via `PromptBuilder` → `LlmClient` (libcurl SSE) → `DialogueManager` → `SentenceSplitter` → `LlmSentence` events → `TurnWriter` → SQLite, and (when `cfg.tts.voices` is non-empty) onward through `TtsBridge` → `PiperClient` → `Resampler` → `PlaybackQueue` → `PlaybackEngine`. M4 adds the capture path: when `cfg.audio.capture_enabled: true`, `CaptureEngine` (PortAudio input) → SPSC ring → `AudioPipeline` worker → `Resampler` (48 → 16 kHz) → `SileroVad` (optional, ONNX Runtime) → `Endpointer` → `UtteranceBuffer` → `SpeechStarted` / `SpeechEnded` / `UtteranceReady` events on the bus. The fake driver gains a `suppress_speech_events` flag so real VAD can own those events while synthetic FinalTranscript/LlmSentence keep flowing for end-to-end smoke tests. Two new demos ship: `acva demo loopback` (mic → speakers passthrough) and `acva demo capture` (mic + VAD endpointing report). Manager enforces `max_assistant_sentences` by cancelling the LLM stream once the cap is hit; `UserInterrupted` drains the bridge's pending queue and clears the playback queue. JSON-per-line logs on stderr; `voice_llm_*` / `voice_health_*` / `voice_pipeline_state` / `voice_llm_keepalive_total` / `voice_tts_first_audio_ms` / `voice_tts_audio_bytes_total` / `voice_playback_{queue_depth,underruns_total,chunks_played_total,drops_total}` emit on `/metrics`; `/status` includes `pipeline_state` + `services[]`. Supervisor probes each backend's `/health`, runs the per-service state machine, gates the dialogue path when a critical backend is unhealthy past the grace window, and runs LLM keep-alive while idle. Next: **M5 — STT** (streaming Whisper via `whisper-server` or Speaches; subscribes to `UtteranceReady` and publishes `PartialTranscript` / `FinalTranscript`).

## Repository Layout

```
src/                     — C++ source. Per-subsystem subdirs: audio/, cli/, config/,
                            dialogue/, event/, http/, llm/, log/, memory/, metrics/,
                            orchestrator/, pipeline/, playback/, stt/, supervisor/, tts/.
src/main.cpp             — slim (~280 lines) linear orchestration: parse args →
                            load config → demo dispatch → build per-subsystem stacks
                            via orchestrator/ helpers → main loop → orderly shutdown.
src/orchestrator/        — host-side glue. One *_stack.{hpp,cpp} per subsystem
                            (tts_stack, capture_stack, stt_stack, dialogue_stack,
                            supervisor_setup) plus bootstrap, event_tracer,
                            status_extra. Each stack is a non-copyable RAII bundle
                            that returns from build_*() with a stop() method that
                            runs the right teardown order.
tests/                   — doctest-based suites: acva_unit_tests (no deps) +
                            acva_integration_tests (real Silero model + future Speaches).
config/default.yaml      — default runtime config (covers everything through M4).
cmake/                   — Dependencies.cmake, Warnings.cmake.
third_party/cpp-httplib/ — vendored single-header HTTP server lib.
scripts/                 — one-shot dev scripts (download-vad.sh, etc).
packaging/
  systemd/               — alternative production deployment: per-user units (M2 stretch / M8).
  compose/               — dev default: docker-compose.yml. Two services: `llama` + `speaches`
                            (Speaches is the OpenAI-API-compat backend that consolidates STT + TTS).
plans/
  project_design.md      — source of truth for architecture, components, milestones, risks.
  open_questions.md      — resolved/unresolved decisions; section L holds implementation-driven revisions.
  milestones/            — one detailed plan per milestone (m0_skeleton.md, m1_llm_memory.md, ...).
  architecture_review.md, local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md — historical inputs.
CMakeLists.txt, CMakePresets.json
README.md, CLAUDE.md, LICENSE, .editorconfig, .gitignore
compile_commands.json    — symlink to _build/dev/compile_commands.json (for clangd).
build.sh                 — `./build.sh [dev|debug|release]`.
run_tests.sh             — `./run_tests.sh [dev|debug]`. Runs the **unit** suite
                            (`acva_unit_tests`): no external deps, fast feedback.
run_integration_tests.sh — `./run_integration_tests.sh [dev|debug]`. Runs the
                            **integration** suite (`acva_integration_tests`): real
                            on-disk assets (Silero model today, more later) and
                            real local services. Resolves dep paths via the same
                            XDG defaults main.cpp uses, so on the dev workstation
                            no env vars are required. Missing deps cause individual
                            cases to skip cleanly, never fail.
src/demos/               — `acva demo <name>` smoke checks per milestone (tone/tts/llm/health/fsm/chat/loopback/capture/stt).
docs/troubleshooting.md  — symptom-first guide; routes failures to the right `acva demo` and reads its output.
```

## Authoritative Documents

- **`plans/project_design.md`** is the source of truth for architecture, components, threading model, latency budget, milestones, and risks. Reference its section numbers when discussing trade-offs.
- **`plans/milestones/m{0..8}_*.md`** are the detailed per-milestone plans. The summary in `project_design.md` §17 links each. **`plans/open_questions.md` section L** holds implementation-driven revisions that supersede earlier sections — read L before assuming an earlier interview answer is still in force.
- **`plans/open_questions.md`** lists unresolved decisions. Before recommending a choice that touches one of these questions, check the default assumption there. If the user is making a real decision, update the question's status in that file.
- The two earlier docs (`architecture_review.md`, `local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md`) are historical inputs; prefer `project_design.md` when they conflict.

## Architectural Pillars (Do Not Violate Without Discussion)

1. **Process isolation for model backends.** llama.cpp, whisper.cpp, Piper run as separate processes the orchestrator does **not** fork. In dev (default since M1.B): Docker Compose containers using upstream images verbatim (`ghcr.io/ggml-org/llama.cpp:server-cuda` etc.). In production (M8 alternative): systemd units with optional sd-bus client (gated by `-DACVA_ENABLE_SDBUS=ON` + `cfg.supervisor.bus_kind`). The orchestrator never `popen`s or `fork()`s a backend. Don't propose embedding model runtimes until post-M8.
2. **Realtime audio path is sacred.** Audio callback never blocks, never allocates, never does I/O or HTTP. SPSC ring buffer is the only mechanism crossing the audio thread boundary.
3. **Cancellation is structural.** Every long-running operation carries a turn ID + cancellation token. TTS audio chunks carry sequence numbers; stale chunks are rejected at enqueue and dequeue. Speculative LLM runs use the same machinery.
4. **Backpressure everywhere.** Every async boundary uses bounded queues with explicit overflow policy.
5. **Observability from day one.** Per-turn trace IDs, structured JSON logs (M1 slice 3 — currently structured plain text), Prometheus metrics on `/metrics`, OTLP traces (opt-in) — not retrofitted later.
6. **Crash recovery is a state machine, not error handling.** SQLite turn lifecycle (`in_progress`, `committed`, `interrupted`, `discarded`) + startup recovery sweep.
7. **Speaker mode + AEC is primary, not optional.** AEC reference-signal alignment is non-negotiable; M6 is a hard prerequisite to M7.
8. **Supervisor observes, Compose acts.** The Supervisor in dev mode runs HTTP `/health` probes and gates the dialogue path; it does NOT issue restart commands — `restart: unless-stopped` in compose handles that. Same Supervisor logic in production with optional sd-bus extension.
9. **No CI.** Tests run locally. Discipline-based; no automated PR gates.

## Tech Stack (Locked for MVP)

- C++23 standard library + language. Lock is specifically against **Boost.Cobalt + C++23 modules**, not C++23 STL features. Glaze 7.x requires C++23 transitively. `std::expected`, `std::print`, deducing `this`, etc. are fair game. Do NOT enable C++23 modules; do NOT use Cobalt.
- Boost.Asio for async, **not** Boost.Cobalt.
- cpp-httplib (vendored at `third_party/cpp-httplib/`) for simple HTTP server + non-streaming client. **libcurl** for SSE streaming (LLM client in M1).
- PortAudio + soxr (M3/M4).
- WebRTC APM (vendored) for AEC/NS/AGC (M6).
- Silero VAD via ONNX Runtime (M4).
- SQLite (WAL mode) + dedicated memory thread (M1).
- glaze for JSON + YAML (config + IPC payloads).
- spdlog (structured-text in M0/M1.A; JSON sink in M1.s3), prometheus-cpp, opentelemetry-cpp (OTLP, opt-in M8).
- Docker Compose for dev backend deployment; **no custom Dockerfiles** in M1.B.
- systemd + libsystemd (sd-bus) — optional production path, gated by `-DACVA_ENABLE_SDBUS=ON`. Not a default M2 dependency.
- doctest for tests.
- CMake + presets.

If you find yourself recommending Boost.Cobalt, C++23 modules, an embedded inference engine for MVP, or a custom HTTP wrapper around a backend that already ships one — re-read `project_design.md` §16 and the open questions first.

## Milestone Order (Adjusted from Original)

`M0 skeleton → M1 LLM+memory (split: A complete, B Compose stack, C remaining) → M2 supervision → M3 TTS+playback → M4 audio+VAD → M4B Speaches consolidation → M5 STT → M6 AEC → M7 barge-in → M8A admin/state → M8B observability/soak → M8C distribution + wake-word → M9 streaming partials + speculative LLM → M10 conversational UX (adaptive endpointer + address detection)`

Three reorderings / insertions vs. the original plan, all intentional:
- **Supervision (M2) before TTS (M3)** — llama.cpp will crash during long-context dev; retrofitting supervision is painful.
- **AEC (M6) before barge-in (M7)** — without AEC the assistant's own voice triggers VAD; you'll spend a week debugging phantom interruptions.
- **M4B Speaches consolidation between M4 and M5** — Speaches packages STT + TTS behind one OpenAI-compatible surface and matches CLAUDE.md pillar #5 ("don't write a custom HTTP wrapper around a backend that already ships one"). Doing this swap *before* M5 closes the M5 L1 decision (A/B/C) up front and lets M5 focus on streaming partials + speculation rather than mixing engine selection in. See `plans/milestones/m4b_speaches_consolidation.md`.

Don't propose moving these back without strong reasons.

**M1 is split** in `plans/milestones/m1_llm_memory.md`: Part A (config, memory, splitter — landed), Part B (Compose stack — next), Part C (LLM client, Dialogue Manager, turn writer, JSON logging — slice 2/3).

## When Working on Code

- **No unbounded queues.** If you need a queue, specify capacity and overflow policy.
- **No blocking calls in the audio callback path.** Even logging — use a lossy queue.
- **Every cancellable operation takes a `TurnContext`.** Don't add a "global cancel flag" or per-component bool.
- **Memory writes go through the memory thread.** Don't open SQLite from random threads — `MemoryThread::submit/read/post` is the only path.
- **HTTP client choice matters.** cpp-httplib for non-streaming, libcurl for SSE. Don't unify them on one library "for simplicity" without measuring.
- **Don't fork backends from the orchestrator.** They run as Compose containers (dev) or systemd units (prod alternative). The orchestrator only opens HTTP connections to them.
- **Don't write a custom HTTP wrapper around a backend that already ships one.** llama.cpp ships `llama-server`; Speaches ships an OpenAI-API-compatible STT + TTS surface that we use for both ends. Streaming-Whisper (`/v1/realtime`, WebRTC) lands in M5.
- **Language flows through the pipeline.** STT detects language → Dialogue Manager passes it to PromptBuilder + TTS. Voice selection is per-language.
- **Errors are silent.** Voice agent never speaks errors to the user. Logs and `/status` are the only error channels (configurable for debug).
- **Tests for the SentenceSplitter must include**: abbreviations (`Dr.`, `e.g.`), decimals (`3.14`), enumerations (`1.`, `2.`), code fences, ellipses, very-long-no-punctuation flush, and **multilingual** boundary cases.
- **Tests for the Dialogue FSM must be table-driven**, asserting all transitions including barge-in cancellation propagation and speculation-reconcile/restart paths.
- **Use `std::variant<T, DbError>` and similar `Result<T>` patterns** for fallible APIs, not exceptions across module boundaries. Internal helpers may throw; public-facing methods return Result.

## Latency Budget (Realistic)

P50 end-to-end (user-stop → first-audio): **~1.7 s.** P95: **~3.5 s.** The original "1–2 s" target was a P50 figure for short prompts. When discussing performance, use percentiles, not point estimates.

## Build & Run

Build:

```sh
./build.sh                # = dev preset; output under _build/dev/
./build.sh debug          # ASan/UBSan-friendly build; _build/debug/
./build.sh release        # -DNDEBUG, tests off; _build/release/
./run_tests.sh            # build + run unit suite for the dev preset
./run_tests.sh dev --test-case='paths*'   # filter pass-through to doctest
./run_integration_tests.sh                # build + run integration suite (Silero, etc.)
```

Equivalent raw cmake invocations (the scripts wrap these):

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run (dev path, with backends in Compose):

```sh
cd packaging/compose && docker compose up -d
cd ../..
./_build/dev/acva                                  # picks up config + db via XDG
```

Run (without backends, M0 fake-driver mode):

```sh
./_build/dev/acva                                  # fake_driver_enabled: true is the default
# in another terminal:
curl http://127.0.0.1:9876/status
curl http://127.0.0.1:9876/metrics
```

**Path resolution (M2.x).** When `--config` is omitted, `config::resolve_config_path` searches in order: `$XDG_CONFIG_HOME/acva/default.yaml` (default `~/.config/acva/default.yaml`), `./config/default.yaml` (in-tree dev fallback), `/etc/acva/default.yaml`. SQLite path: `cfg.memory.db_path` empty/relative resolves to `$XDG_DATA_HOME/acva/<value>` (default `~/.local/share/acva/acva.db`). Parent dirs are auto-created on first run. Tests in `tests/test_paths.cpp` cover the precedence + fallback rules.

The `acva` binary itself always runs on the host as a CLI process — it's intentionally never put inside Compose so the realtime audio path stays direct. Production-style packaging as `acva.service` is M8 work.

## Claude-Specific Working Notes

- Prefer editing `plans/project_design.md`, `plans/open_questions.md`, and the relevant `plans/milestones/m*_*.md` over creating new design docs.
- When user asks to "remember" a decision about an open question, update `plans/open_questions.md` (mark resolved, record the answer) — that's project state, not memory. New revisions go in section L.
- When the user asks for design changes, edit `plans/project_design.md` and the affected milestone plans directly; don't write a separate change-proposal file.
- Don't generate planning/decision/analysis docs unless explicitly requested.
- After each implementation slice, update the relevant milestone plan to mark steps as ✅ landed and capture lessons learned (see `m0_skeleton.md` and `m1_llm_memory.md` Part A as templates).
- When clangd flags errors that look like missing-include cascades, the most common cause is a stale `compile_commands.json`. The build itself is the truth — a clean `cmake --build --preset dev` is the verdict, not editor diagnostics.
