# Open Questions

Decisions to make before or during implementation. Grouped by area.

## Status

After the Section A–K interview, **all original questions are resolved**. New questions surfaced during the interview (B7–B10, C7, E5b, H6–H7) are listed inline and either resolved or carry default assumptions.

Resolved decisions are marked *(resolved)* with the chosen answer and consequence. Unresolved entries (`B8`, `B10`, `H6`, `H7` — minor implementation details) keep their default assumption and impact note for future handling.

Major shifts from the original design that came out of this interview:

- **Speakers + AEC is the primary UX** (was: both, headphones reliable until M6).
- **Streaming partial STT + speculative LLM in MVP** (was: utterance-based, deferred). *Note: L1 below resolved to option B — Speaches — and escalated into a dedicated **M4B** milestone for voice-backend consolidation. M5 keeps streaming + speculation as planned.*
- **Full multilingual** STT/TTS/LLM with per-utterance language detection (was: English-only).
- **Docker Compose for backends in dev; systemd as alternative production path** (was: systemd-managed services from day one). See L0 below.
- **OTLP traces** opt-in (was: logs only).
- **No CI** — local-only testing.
- **soxr** instead of libsamplerate.
- **Boost.Asio** instead of standalone asio (Boost is now a project dep).
- **Tool calling fully out of scope** — no architectural reservation.
- **Errors never spoken** — logs and `/status` only.

Schedule shifted from ~12–14 weeks to **~14–16 weeks** because of M5 expansion. M3 trimmed by ~1 week (no Piper wrapper); M2 trimmed by ~4 days (no sd-bus by default); M1 added ~1 day (Compose stack setup). Net: roughly the same total, with significantly less custom infrastructure.

---

## A. Hardware & Deployment

**A1. Headphones or speakers as the default UX?** *(resolved)*
- Decision: **speakers**, AEC mandatory.
- Consequence: M6 (AEC) is a hard prerequisite to M7 (barge-in) — already the order. AEC reference-signal alignment must be tight; AEC delay estimate is a first-class metric. Speaker-mode barge-in reliability is a primary success criterion, not "≥ 80% with AEC".

**A2. Single-machine deployment only, or multiple workstations?** *(resolved)*
- Decision: **single machine only**.
- Consequence: all services on `127.0.0.1`. No auth, no TLS on local IPC. Service discovery is config-file paths.

**A3. Background usage during active GPU workloads (gaming, video work)?** *(resolved)*
- Decision: **exclusive GPU access required**.
- Consequence: documented as a precondition; no VRAM-pressure detection or graceful degradation in MVP. If GPU is contended, behavior is "errors, possibly crashes" — handled by supervisor restarts but not gracefully.

---

## B. Models & Inference

**B1. Whisper on GPU or CPU?** *(resolved)*
- Decision: **CPU**.
- Consequence: ~1 GB VRAM saved for Qwen. STT latency higher (~600–1000 ms with small) — partially offset by speculative LLM start (B5).

**B2. Qwen2.5-7B Q4_K_M vs Q5_K_M vs Q4_K_S?** *(resolved)*
- Decision: **Q4_K_M** (~4.5 GB).
- Consequence: standard balance. Q5_K_M can be added later as opt-in if quality demands it.

**B3. Whisper model size?** *(resolved)*
- Decision: **configurable, default small** (multilingual).
- Consequence: small is the recommended default; users can switch to base (faster, less accurate) or medium (slower, more accurate). Config gate exists from M5.

**B4. Piper voice model selection?** *(resolved)*
- Decision: **per-language voice pack with lazy load**, ~500 MB voice memory budget by default.
- Consequence: config maps language → voice path. Voices loaded on first use; LRU eviction over budget. Aligns with B6 multilingual decision.

**B5. Streaming partial STT — defer to post-MVP?** *(resolved)*
- Decision: **in-MVP**.
- Consequence: M5 scope expands to 2–3 weeks. Dialogue FSM grows a `SpeculativeThinking` concurrent sub-state (§6). Speculation policy is configurable. Speculation can only emit to TTS after `FinalTranscript` confirms — protects user from hearing mismatched answers.

**B6. English-only or multilingual?** *(resolved)*
- Decision: **full multilingual** (STT + TTS + LLM).
- Consequence: multilingual Whisper, per-language Piper voices, language flows through prompt + TTS request as a first-class field. Memory schema gains a `lang` column on turns (see §9.1 update needed).

**B7 (new). How is the active language determined per turn?** *(resolved)*
- Decision: **auto-detect from STT output**.
- Consequence: Whisper detects language per utterance and emits it on `FinalTranscript`. Dialogue Manager passes it to PromptBuilder (system-prompt instruction "reply in {lang}") and to TTS (voice selection).

**B8 (new). Speculation policy thresholds — what defaults?**
- Default assumption: `speculation_hangover_ms=250`, `speculation_min_chars=20`, `speculation_stability_ms=200`, `speculation_match_ratio=0.9`. Cap speculative restarts per minute to prevent thrash.
- Impact: too aggressive → wasted GPU cycles and possible visible glitches; too conservative → no latency benefit. Tune empirically during M5.

**B9 (new). Memory in multilingual context — store turns in original language or normalize?**
- Default assumption: store turns in their **original language**; add a `lang` column. Summaries written in the dominant language of the source range (Whisper-detected). Facts stored language-tagged.
- Impact: prompt assembly should not auto-translate; LLM handles cross-language context natively.

**B10 (new). Whisper streaming — reuse existing whisper.cpp `stream` example or integrate at library level?**
- Default assumption: wrap the streaming approach as a small service binary (HTTP API for partials + finals). Reuse whisper.cpp's `stream` algorithm (sliding window with N seconds context).
- Impact: writing a streaming HTTP wrapper around whisper.cpp is non-trivial; the project may need a custom whisper service binary instead of using a stock one. Allow ~1 week of M5 for this.

---

## C. Audio

**C1. Resampler choice: libsamplerate, speex resampler, or soxr?** *(resolved)*
- Decision: **soxr**, used everywhere.
- Consequence: highest quality; phase-stable for AEC. One library = one failure mode.

**C2. Audio backend: PortAudio or RtAudio?** *(resolved)*
- Decision: **PortAudio**.

**C3. AEC reference signal source?** *(resolved)*
- Decision: **in-process post-resampler tap**.
- Consequence: requires careful frame alignment between mic stream and the loopback tap point. AEC delay estimate becomes a first-class metric. The tap is taken *after* the playback resampler so AEC sees what the speaker actually emits.

**C4. VAD hangover defaults?** *(resolved)*
- Decision: **600 ms hangover, 200 ms minimum utterance**.
- Consequence: configurable; tune on real audio fixtures during M4. Note: speculation hangover (B8) is shorter (250 ms) and runs in parallel.

**C5. Trigger mode for Phase 1 (pre-wake-word)?** *(resolved)*
- Decision: **always-on VAD with mute hotkey**.
- Consequence: AEC + multilingual VAD is mandatory to keep false-triggers in check. Mute hotkey implementation is itself an open question (see I5).

**C6. Sample rates?** *(resolved)*
- Decision: **48 kHz hardware, 16 kHz internal**.
- Consequence: single soxr resample at capture entry; playback resampler converts 22.05 kHz Piper → 48 kHz speaker.

**C7 (new). Audio device selection policy?** *(resolved)*
- Decision: **OS default; configurable override by name or index**.
- Consequence: simple, predictable. Hot-plug detection (USB headset reconnect) is post-MVP.

---

## D. Concurrency & I/O

**D1. asio: standalone or Boost.Asio?** *(resolved)*
- Decision: **Boost.Asio**.
- Consequence: Boost is now a project dependency. This implicitly enables boost::beast, boost::lockfree etc. if needed later (we still chose libcurl and hand-rolled SPSC). Build system must locate Boost ≥ 1.83.

**D2. SSE client?** *(resolved)*
- Decision: **libcurl directly** for SSE; **cpp-httplib** for non-streaming HTTP.
- Consequence: two HTTP libraries on purpose. cpp-httplib is header-only and trivial; libcurl handles streaming with reliable cancellation.

**D3. Lock-free SPSC ring?** *(resolved)*
- Decision: **hand-roll**.
- Consequence: ~100 lines, fixed-size, atomic head/tail, padded to avoid false sharing. Tailored to our 10 ms frame format. Dedicated unit tests for it (memory ordering matters).

**D4. JSON library: nlohmann or glaze?** *(resolved)*
- Decision: glaze, used for both JSON and YAML.
- Rationale: one library, reflective struct binding, fast. Trade-off accepted: newer than nlohmann, smaller ecosystem.

---

## E. Memory & Persistence

**E1. Record raw audio per turn?** *(resolved)*
- Decision: **off by default, configurable**.
- Consequence: when on, audio path stored in `turns.audio_path`. Retention is also configurable: rolling N hours / per-session / wipe-on-session-end (default rolling 24 h when enabled).

**E2. Encryption-at-rest for SQLite?** *(resolved)*
- Decision: **none**.
- Consequence: plain SQLite WAL files. Document that the DB and any audio are unencrypted on disk.

**E3. Session boundary?** *(resolved)*
- Decision: **idle timeout** (default 30 min) **+ explicit `/new-session` command**.
- Consequence: each session has its own ID; memory boundaries follow sessions; summaries are per-session.

**E4. Facts extraction policy?** *(resolved — configurable)*
- Decision: **configurable**, default **conservative** ("only directly stated").
- Knobs: `memory.facts.policy: conservative | moderate | aggressive | manual_only`; `memory.facts.confidence_threshold: 0.7`.
- Consequence: ship with conservative; provide moderate/aggressive presets and a `manual_only` mode that disables auto-extraction entirely.

**E5. Summary regeneration trigger?** *(resolved — configurable)*
- Decision: **configurable**, default **every 15 new turns since last summary**.
- Knobs: `memory.summary.trigger: turns | tokens | idle | hybrid`; `memory.summary.turn_threshold: 15`; `memory.summary.token_threshold: 4000`; `memory.summary.idle_seconds: 120`.
- Consequence: summarization runs on the memory thread, never blocks Dialogue. Source-hash check triggers refresh independently when source turns change.

**E5b (new). Summary language strategy?** *(resolved — configurable)*
- Decision: **configurable**, default **dominant language of source range**.
- Knobs: `memory.summary.language: dominant | english | recent`.
- Consequence: `summaries.lang` column stores the chosen language. Prompt assembly passes summary-as-is to the LLM regardless of current turn language.

**E6. Memory bootstrap on first launch?** *(resolved)*
- Decision: **optional YAML profile seed**.
- Consequence: empty by default; if `user_profile.yaml` exists at the configured path, pre-populate `facts` and `settings` from it on first launch (idempotent: re-running with the same file is a no-op, second run with new keys adds them).

---

## F. Dialogue & Prompting

**F1. System prompt location?** *(resolved)*
- Decision: **in YAML config**. Hot-reloadable.
- Consequence: per-language system prompts can be configured under `dialogue.system_prompts.{lang}:`.

**F2. Tool calling reservation?** *(resolved — postponed)*
- Decision: **out of scope for MVP**. No architectural reservation; revisit entirely as a post-MVP follow-up.
- Consequence: Dialogue Manager does not need to inspect LLM output for tool-call markers. SentenceSplitter handles raw text. K3 (security model for tool execution) is also deferred.

**F3. Maximum prompt length policy?** *(resolved — configurable)*
- Decision: **configurable, default 3000 tokens**.
- Knob: `llm.max_prompt_tokens: 3000`. When prompt assembly hits the cap, the prompt builder triggers summarization-then-shrink.
- Consequence: this is the lever for first-token latency vs. recall trade-off.

**F4. Clarifying-question policy?** *(resolved)*
- Decision: **at most one per turn, prompt-enforced**.
- Consequence: not structurally enforced; rely on Qwen's instruction-following. Document as "best effort" in tests.

**F5. Spoken-style enforcement?** *(resolved)*
- Decision: **prompt only** for MVP.
- Consequence: ship with strong system-prompt language ("answer in short spoken paragraphs, no lists, no markdown"). If observed non-compliance becomes a real issue, a post-processor can be added later.

---

## G. Cancellation & Backpressure

**G1. LLM cancellation on UserInterrupted?** *(resolved)*
- Decision: **actively cancel via libcurl abort**.
- Consequence: llama.cpp handles client disconnect by stopping generation. Saves GPU cycles; reduces the cancellation race window.

**G2. TTS cancellation policy?** *(resolved)*
- Decision: **wait + drop**.
- Consequence: in-flight Piper synthesis completes; the audio chunk is rejected at playback enqueue (turn-ID mismatch). Slight CPU waste for the rare long-sentence case; acceptable simplicity.

**G3. Hard cap on assistant response length?** *(resolved)*
- Decision: **6 sentences default, configurable**.
- Knobs: `dialogue.max_assistant_sentences: 6`; `dialogue.max_assistant_tokens: 400` (secondary cap).
- Consequence: enforced by Dialogue Manager — when reached, sends LLM cancel signal, finishes the current sentence, transitions to Listening.

---

## H. Observability & Operations

**H1. Metrics export?** *(resolved)*
- Decision: **Prometheus on `/metrics`** (HTTP, localhost) via `prometheus-cpp`.

**H2. Tracing?** *(resolved)*
- Decision: **OTLP traces** via `opentelemetry-cpp`.
- Consequence: adds a dependency. OTLP endpoint is opt-in via config (`observability.otlp.endpoint`). Recommended target: a local otelcol-contrib instance. When disabled, tracing degrades gracefully to JSON logs only.

**H3. Crash dump policy?** *(resolved)*
- Decision: **rely on systemd-coredump**.
- Consequence: no custom signal handler. systemd journal will reference the dump. Document `coredumpctl list acva` for users.

**H4. Distribution model?** *(resolved — implied by H5)*
- Decision: **service-set with systemd units**, not a single self-launching binary.
- Consequence: package installs `acva` orchestrator binary + systemd unit files for `acva.service`, `acva-llama.service`, `acva-whisper.service`, `acva-piper.service`. User-level units (`systemd --user`) are the default; system-wide units are supported.

**H5. External services launched by whom?** *(resolved)*
- Decision: **systemd**. Orchestrator manages units (start/stop/restart/status), does not fork child processes.
- Consequence: §4.12 Supervisor rewritten. systemd handles process lifecycle; orchestrator handles application-level health and coordination. Linux-only deployment (already implicit).

**H6 (new). systemd interaction — sd-bus or `systemctl` subprocess?**
- Default assumption: **sd-bus** (libsystemd) for runtime interaction; `systemctl` only for one-off install/uninstall scripts.
- Impact: sd-bus is faster, structured, and async-friendly via Boost.Asio file descriptors. Adds `libsystemd-dev` build dep.

**H7 (new). OTLP collector packaging — do we ship one?**
- Default assumption: no. Document recommended config for otelcol-contrib in README; user installs separately. OTLP off by default.
- Impact: bundling a collector would simplify "trace works out of the box" but adds binary size.

---

## I. UX & Control Plane

**I1. GUI for MVP?** *(resolved)*
- Decision: **headless + HTTP control plane** on localhost.
- Endpoints: `/status`, `/reload`, `/mute`, `/unmute`, `/new-session`, `/wipe`, `/metrics`.

**I2. Mute hotkey?** *(resolved)*
- Decision: **HTTP endpoint, user wires their own hotkey** via compositor.
- Consequence: README documents how to bind a hotkey under sway/hyprland/GNOME/KWin/i3 to `curl -X POST localhost:PORT/mute`.

**I3. Error feedback?** *(resolved)*
- Decision: **always silent — logs only**.
- Consequence: errors never spoken to user. `ux.speak_errors: false` is the only setting; debug flag can enable for development.
- Note: this is a deliberate UX choice. If field testing shows users miss errors, revisit.

**I4. Conversation export?** *(resolved)*
- Decision: **text export via CLI**, markdown by default, JSON optional.
- CLI: `acva export --session <id> [--format markdown|json]`.

---

## J. Testing & Validation

**J1. Soak test environment & CI strategy?** *(resolved)*
- Decision: **no CI**. All testing is local on developer's machine.
- Consequence: discipline-based; tests must pass locally before merging. Soak tests run manually before releases. No automated PR gating, no GitHub Actions, no nightly runs.
- Risk: "works on my machine" drift across collaborators. Mitigated by single-developer assumption for MVP; revisit if team grows.

**J2. Audio fixture corpus?** *(resolved)*
- Decision: **LibriSpeech + Common Voice from day one**, plus hand-recorded edge cases (noise, echo, accents, code-switching).
- Consequence: license-compliant subsets included via a download script (not checked-in binaries). Per-language WER thresholds documented. Multilingual coverage from start aligns with B6 decision.

**J3. Latency-regression detection?** *(resolved)*
- Decision: **manual review of soak summary**.
- Consequence: developer reads P50/P95 numbers from soak runs. No automated gate. Acceptable for solo MVP development.

---

## K. Strategic / Future-Proofing

**K1. Embed model runtimes ever?** *(resolved)*
- Decision: **revisit after M8; may never**.
- Consequence: no in-process model assumption baked into APIs. External services are the architecture indefinitely unless measured benefit demands embedding.

**K2. Streaming end-to-end speech model compatibility?** *(resolved)*
- Decision: **compatible by replacing STT+LLM stages**; no architectural changes today.
- Consequence: when such a model emerges (Qwen-Audio etc.), STT and LLM service interfaces collapse into one. VAD/Dialogue/TTS/Playback stay intact. Document this future path; no code now.

**K3. Tool calling — timing and security model?** *(resolved)*
- Decision: **defer entirely; revisit post-MVP**. When added: sandboxed execution (firejail or bubblewrap) + per-call user approval.
- Consequence: aligns with F2. No `ToolExecutor` slot reserved now.

---

## L. Implementation-driven revisions

Decisions that surfaced during M0/M1 implementation, post-interview. These supersede earlier sections where they conflict.

**L0. Backend deployment for dev — Compose vs systemd.** *(resolved)*
- Decision: **Docker Compose for the dev path; systemd as an alternative production path (M8).**
- `acva` itself runs as a host CLI binary throughout MVP — including production via the systemd path, where it gets its own `acva.service` unit.
- Compose runs three services using **upstream images verbatim** (`ghcr.io/ggml-org/llama.cpp:server-cuda`, `ghcr.io/ggml-org/whisper.cpp:server`, `python:3.12-slim` + `pip install piper-tts`). No custom Dockerfiles in M1.
- Models live on the host under `~/.local/share/acva/{models,voices}/` and are bind-mounted into containers read-only.
- Consequences:
  - M2 simplifies dramatically (HTTP probes only; Compose owns process lifecycle). Estimate dropped from ~1 week to ~3 days.
  - M3 loses the Piper-wrapper subproject (~2 days saved). Per-language voices now mean per-language Compose services.
  - M5's custom Whisper streaming wrapper gets reframed as one of three options — see L1.
  - `packaging/systemd/` stays in tree as production deployment alternative; not the default.

**L1. M5 streaming-STT engine.** *(resolved → option B, escalated to M4B)*
- Decision: **B (Speaches).** Inserted as a dedicated milestone **M4B** between M4 and M5 because Speaches packages STT + TTS together and the right move is to consolidate the *entire* voice backend, not just STT. M5 then becomes a focused "streaming + speculation" task against an already-chosen and smoke-tested engine.
- Original three options preserved here for context:
  - **A.** Custom C++ wrapper around `whisper.cpp/examples/stream` (~3 weeks).
  - **B.** Adopt **Speaches** / faster-whisper, OpenAI-Realtime-compatible streaming server (~2 weeks).
  - **C.** Defer streaming partials past MVP (~1 week); drops `SpeculativeThinking` from the FSM.
- Why B + M4B over the original "decide at M5 start": Speaches ALSO ships Piper TTS behind the same OpenAI-compatible surface. Adopting it for STT but keeping standalone `piper.http_server` for TTS would leave us with two HTTP-client styles in tree and three Compose services where two would do. M4B does the consolidation; M5 layers streaming on top.
- Fallback if M4B's smoke gate fails (Speaches Piper support broken / project stagnated): revert to option **A** at M5 start, keep the existing `piper.http_server` and `whisper.cpp/server` services. M4B's plan documents this fallback explicitly.
- Consequences:
  - M5 estimate drops from 2–3 weeks to **1.5–2 weeks**: engine selection is done, request/response STT client already shipped (in M4B), streaming becomes a swap of one client implementation against the same event surface.
  - Compose collapses from `llama` + `whisper` + `piper` to `llama` + `speaches`.
  - `cfg.tts.voices[*].url` → `cfg.tts.base_url` + per-language `voice_id`. One-time config migration; no production deploys yet so this is acceptable.
- See `plans/milestones/m4b_speaches_consolidation.md` for the full plan.

**L2. C++23 baseline, not C++20.** *(resolved retroactively)*
- The C++20 lock from B-section was inadvertently violated by glaze 7.x, which forces `-std=c++23` transitively. C++23 STL features (std::expected, deducing-this, etc.) are now fair game.
- Cobalt and C++23 modules remain forbidden — that part of the original lock stands.

**L3. JSON logging deferred from M0 to M1 slice 3.** *(resolved)*
- spdlog 1.17 has no built-in `%j` JSON-escape format flag; we'd need a custom sink. M0 ships structured-text logs; M1 slice 3 implements the JSON sink.
- Consequence: log lines through M0 and early M1 are not strict JSON. Greppable but not parseable as one-event-per-line JSON until M1.s3.

**L4. M5 STT client is session-scoped, not per-utterance.** *(resolved)*
- The original M5 plan sketched `WhisperClient::begin/push_audio/end` as a per-utterance API in which each utterance owned its own backend connection. The realtime spike (2026-05-02) pinned the actual contract: Speaches' `/v1/realtime` is a long-lived WebRTC session in which utterances are delineated by `input_audio_buffer.commit` events on the same data channel. Re-negotiating SDP per utterance would add ~500 ms of ICE+DTLS latency to every turn for no gain.
- Decision: `RealtimeSttClient` owns one long-lived `rtc::PeerConnection` per app run. The public per-utterance API (`begin/push_audio/end`) — landing in M5 Step 2.b — translates each utterance into `input_audio_buffer.append` + `commit` on the shared session. Reconnect-on-failure is the supervisor's job; the client surfaces `State::Failed` on disconnect.
- Consequence: `start()` is a one-shot bring-up at orchestrator startup, not a per-turn cost. The 15 s default timeout includes ICE gathering + SDP exchange + DTLS + `session.updated` round-trip; measured ~2 s end-to-end on the dev workstation.

**L5. Server VAD parked at threshold=1.0; client owns explicit `input_audio_buffer.commit`.** *(resolved)*
- Speaches' `PartialSession` (the schema for `session.update`) types `turn_detection` as `TurnDetection | NotGiven` — `null` is rejected at validation time, and there is no "off" / "none" variant.
- The earlier "let server VAD auto-commit" plan failed in production: with the server-locked `prefix_padding_ms: 0`, server VAD fires rapid-fire `speech_started`/`speech_stopped` pairs at ~10 ms intervals during real (live mic) speech. Each `speech_stopped` auto-publishes `input_audio_buffer.committed`, and the resulting back-to-back commit storm produced "Error adding item: the item ... already exists" failures from the conversation router. The realtime smoke test masked this because its fixture pads 800 ms of trailing silence (single clean `speech_stopped`); production mic input has no such pad.
- **Final decision:** session.update sets `turn_detection.threshold = 1.0` so server VAD never fires. Other TurnDetection fields are sent at their existing defaults (`prefix_padding_ms: 0`, `silence_duration_ms: 550`, `type: server_vad`) because Speaches' schema marks them all required — `prefix_padding_ms` triggers an "unsupported field" error event, which the client logs but doesn't treat as fatal (the rest of the update applies). `create_response: false` stops Speaches from auto-running chat-completion against its own (un-hosted) `/v1/chat/completions`.
- With server VAD silenced, `RealtimeSttClient::end_utterance()` sends `input_audio_buffer.commit` itself on each M4 `SpeechEnded`. The server then publishes `input_audio_buffer.committed` and runs transcription on the buffer we appended.
- Speaches' `input_audio_buffer.speech_started` / `.speech_stopped` events never fire under threshold=1.0 (no speech ever crosses the threshold). Our M4 Silero owns the bus-level `SpeechStarted` / `SpeechEnded` events that drive the dialogue FSM.
- Bonus side-effect of disabling server VAD: `data_w_vad_applied` returns the entire buffer (no audio_start_ms set), so leading-phoneme trimming we'd seen earlier is gone. Whisper sees the full M4-padded utterance.

**L6. M5 streaming partials + speculation lifted into M9.** *(resolved — scope decision)*
- Speaches' realtime endpoint as of 2026-05-02 doesn't emit `conversation.item.input_audio_transcription.delta` events: `realtime/input_audio_buffer.py` awaits the full WAV via `transcription_client.create(...)` after `commit` and publishes a single `transcription.completed`.
- The original M5 Steps 4–5 (FSM `SpeculativeThinking` + `SpeculationGate`) plus the matching acceptance gates (median first-token-ready 300 ms savings, mid-utterance revision, `voice_speculation_*` counters) all depend on `PartialTranscript`. Without partials they're unreachable on Option B.
- Decision: lift those steps and gates into a new milestone **M9** (`plans/milestones/m9_speculation.md`). M5 ships as "streaming session, no partials" with the speech-to-speech loop closing on `FinalTranscript`. M9 picks up speculation when a partial-transcript source is in place — three options enumerated in the M9 plan (PR Speaches, side-car streaming Whisper, re-platform STT).
- This is a real scope change vs. the original L1 decision, but it reflects the cost of either alternative. Switching to Option A (custom whisper.cpp wrapper) at this point in M5 implementation would have been multi-week scope creep; PRing Speaches has uncertain merge timing. Better to ship M5–M8 against what Speaches delivers today and address speculation as its own milestone with its own dependency-research step.

**L7. M6B closed via PipeWire system AEC; in-process APM stays disabled-by-default on this hardware class.** *(resolved 2026-05-04)*
- Decision: ship `cfg.apm.use_system_aec: true` as the default. The in-process WebRTC APM wiring (M6 § 5.1) is correct but ineffective on the dev laptop's ALC257 codec + class-D speaker chain (M6 § 5.2 + `docs/aec_report.md` § 6). PipeWire's `module-echo-cancel` consumes the raw ALSA mic stream upstream of the codec DSP and gives 25–46 dB speech-band cancellation on the same hardware (M6B Step 4.1).
- Two non-obvious follow-ons were required before gate 1 would PASS — record here so a future reader doesn't re-debug them:
  - **`system_aec.cpp` PA name parsing on module reuse.** When acva starts and finds an existing `module-echo-cancel`, it must parse `source_name=` / `sink_name=` from the args column of `pactl list short modules` and use those exact names in `PULSE_SINK` / `PULSE_SOURCE`. The pre-fix code hardcoded `echo-cancel-{source,sink}` (PipeWire's no-args defaults), which silently mismatched any acva-loaded module from a previous run and routed audio around the AEC. Symptom was gate-1 hitting 33/min false-starts despite the module being loaded. The helper now also adopts ownership when names match the `acva-echo-*` convention (so a crashed run gets cleaned up on the next clean exit) and refuses to start when args are unparseable rather than falling back silently.
  - **`WHISPER__TTL=-1` in compose.** Speaches' default 5-min auto-evict combined with faster-whisper [#992](https://github.com/SYSTRAN/faster-whisper/issues/992) leaks ~300 MB of CUDA memory per unload cycle. With llama (~5 GB) + turbo Whisper (~1.3 GB) + Piper voices already filling ~6.8 GB on the 8 GB RTX 4060, four reload cycles take VRAM to 7.8 GB and the next inference allocation OOMs. Pinning TTL to -1 is what makes back-to-back integration tests / long soaks stable. Also a UX win: no cold-load latency on the first user utterance after an idle period.
- Path A (in-process APM) stays compiled and gated by `cfg.apm.aec_enabled`. Both stacks running simultaneously is documented as a misconfiguration (NS over-suppresses, two AGCs fight, AEC convergence on a pre-cleaned signal zeroes the first ~800 ms of every utterance — observed during M6B exploration; see `docs/aec_report.md` § 10.5).
- M7 (barge-in) is now unblocked. The half-duplex gate (`cfg.audio.half_duplex_while_speaking`) ships `false` and is no longer the contingency on Linux/PipeWire desktops; it remains as the documented production default for hardware where Path B isn't available.
