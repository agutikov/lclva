# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

**lclva** — Long Conversation Local Voice Agent. A local, production-grade C++ voice assistant for multi-hour conversations on a single workstation (RTX 4060). All inference is local; model runtimes (llama.cpp, whisper.cpp, Piper) run as **systemd-managed services**; the C++ orchestrator owns control plane, audio I/O, state, memory, and observability.

Target hardware: Linux x86_64, RTX 4060 8 GB. Speakers + AEC is the primary UX (not headphones).
Multilingual STT/TTS/LLM with auto language detection per turn.
Streaming partial STT with speculative LLM start.

## Status

Pre-implementation. The repo currently contains design documents only — no source tree yet. Implementation is planned to begin from Milestone 0 (skeleton runtime) per `plans/project_design.md`.

## Repository Layout

```
plans/
  project_design.md       — full consolidated architecture + 9 milestones + risk register
  open_questions.md       — decisions to make; each has a default assumption + impact note
  architecture_review.md  — first review of the original design
  local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md — original design doc
README.md
LICENSE
.gitignore                — set up for C/C++ + CMake
```

When code lands, expect: `src/`, `include/`, `tests/`, `config/`, `CMakeLists.txt`, `CMakePresets.json`.

## Authoritative Documents

- **`plans/project_design.md`** is the source of truth for architecture, components, threading model, latency budget, milestones, and risks. Reference its section numbers when discussing trade-offs.
- **`plans/open_questions.md`** lists unresolved decisions. Before recommending a choice that touches one of these questions, check the default assumption there. If the user is making a real decision, update the question's status in that file.
- The two earlier docs (`architecture_review.md`, `local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md`) are historical inputs; prefer `project_design.md` when they conflict.

## Architectural Pillars (Do Not Violate Without Discussion)

1. **Process isolation for model runtimes.** llama.cpp, whisper.cpp, Piper run as **systemd units**. The orchestrator manages them via sd-bus (start/stop/restart/status), does **not** fork child processes. Don't propose embedding model runtimes until post-M8.
2. **Realtime audio path is sacred.** Audio callback never blocks, never allocates, never does I/O or HTTP. SPSC ring buffer is the only mechanism crossing the audio thread boundary.
3. **Cancellation is structural.** Every long-running operation carries a turn ID + cancellation token. TTS audio chunks carry sequence numbers; stale chunks are rejected at enqueue and dequeue. Speculative LLM runs use the same machinery.
4. **Backpressure everywhere.** Every async boundary uses bounded queues with explicit overflow policy.
5. **Observability from day one.** Per-turn trace IDs, structured JSON logs (to journald), Prometheus metrics on `/metrics`, OTLP traces (opt-in) — not retrofitted later.
6. **Crash recovery is a state machine, not error handling.** SQLite turn lifecycle (`in_progress`, `committed`, `interrupted`, `discarded`) + startup recovery sweep.
7. **Speaker mode + AEC is primary, not optional.** AEC reference-signal alignment is non-negotiable; M6 is a hard prerequisite to M7.
8. **No CI.** Tests run locally. Discipline-based; no automated PR gates.

## Tech Stack (Locked for MVP)

- C++20 (not C++23 — see project_design.md §16 for why; revisit after M8)
- Boost.Asio for async, **not** Boost.Cobalt
- cpp-httplib for simple HTTP, **libcurl** for SSE streaming
- PortAudio + soxr
- WebRTC APM (vendored) for AEC/NS/AGC
- Silero VAD via ONNX Runtime
- SQLite (WAL mode) + dedicated memory thread
- glaze for JSON + YAML (config + IPC payloads)
- spdlog (JSON sink), prometheus-cpp, opentelemetry-cpp (OTLP)
- systemd unit management via sd-bus (libsystemd)
- doctest or Catch2 for tests
- CMake + presets

If you find yourself recommending Boost.Cobalt, C++23 modules, or an embedded inference engine for MVP — re-read `project_design.md` §16 and the open questions first.

## Milestone Order (Adjusted from Original)

`M0 skeleton → M1 LLM+memory → M2 supervision → M3 TTS+playback → M4 audio+VAD → M5 STT → M6 AEC → M7 barge-in → M8 hardening`

Two reorderings vs. the original plan, both intentional:
- **Supervision (M2) before TTS (M3)** — llama.cpp will crash during long-context dev; retrofitting supervision is painful.
- **AEC (M6) before barge-in (M7)** — without AEC the assistant's own voice triggers VAD; you'll spend a week debugging phantom interruptions.

Don't propose moving these back without strong reasons.

## When Working on Code

- **No unbounded queues.** If you need a queue, specify capacity and overflow policy.
- **No blocking calls in the audio callback path.** Even logging — use a lossy queue.
- **Every cancellable operation takes a `TurnContext`.** Don't add a "global cancel flag" or per-component bool.
- **Memory writes go through the memory thread.** Don't open SQLite from random threads.
- **HTTP client choice matters.** cpp-httplib for non-streaming, libcurl for SSE. Don't unify them on one library "for simplicity" without measuring.
- **Process management is via sd-bus, not fork/exec.** Don't propose `popen` or `fork()` to launch llama.cpp/whisper.cpp/Piper.
- **Language flows through the pipeline.** STT detects language → Dialogue Manager passes it to PromptBuilder + TTS. Voice selection is per-language.
- **Errors are silent.** Voice agent never speaks errors to the user. Logs and `/status` are the only error channels (configurable for debug).
- **Tests for the SentenceSplitter must include**: abbreviations (`Dr.`, `e.g.`), decimals (`3.14`), enumerations (`1.`, `2.`), code fences, ellipses, very-long-no-punctuation flush, and **multilingual** boundary cases.
- **Tests for the Dialogue FSM must be table-driven**, asserting all transitions including barge-in cancellation propagation and speculation-reconcile/restart paths.

## Latency Budget (Realistic)

P50 end-to-end (user-stop → first-audio): **~1.7 s.** P95: **~3.5 s.** The original "1–2 s" target was a P50 figure for short prompts. When discussing performance, use percentiles, not point estimates.

## Build & Run

Not yet implemented. Once the build exists, expected commands:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/lclva --config config/default.yaml
```

External services (llama.cpp, whisper.cpp, Piper) run as **systemd units** (`lclva-llama.service`, `lclva-whisper.service`, `lclva-piper.service`); the orchestrator interacts with them via sd-bus. Per-user mode (`systemctl --user`) is the default deployment.

## Claude-Specific Working Notes

- Prefer editing `plans/project_design.md` and `plans/open_questions.md` over creating new design docs.
- When user asks to "remember" a decision about an open question, update `plans/open_questions.md` (mark resolved, record the answer) — that's project state, not memory.
- When the user asks for design changes, edit `plans/project_design.md` directly; don't write a separate change-proposal file.
- Don't generate planning/decision/analysis docs unless explicitly requested.
- Don't add C++ source files until the user asks to start implementation. Design phase is not done.
