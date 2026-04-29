# Local Voice AI Orchestrator — Consolidated Project Design & Plan

Synthesis of:
- `local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md` (original design)
- `architecture_review.md` (first review)
- Second-pass analysis (this document's author)

Open questions are tracked separately in `plans/open_questions.md`.

---

## 1. Project Goal

A local, production-grade, configurable voice assistant for long (30 min – multi-hour) conversations on a single workstation with an RTX 4060 (8 GB VRAM). The system must support reliable barge-in, bounded latency, crash recovery, and observability from day one.

Hardware target:
- GPU: RTX 4060 8 GB
- CPU: modern x86_64 (≥ 8 cores assumed)
- OS: Linux (Manjaro/Arch family primary; Ubuntu LTS secondary)
- Audio: USB or built-in mic + speakers/headphones

Non-goals for MVP: multi-user, mobile, custom model training, full-duplex perfection, emotional prosody.

---

## 2. Architectural Pillars

These are the load-bearing decisions. Everything else is consequence.

1. **Process isolation for model runtimes.** llama.cpp, whisper.cpp, Piper run as separate processes. The C++ orchestrator owns control plane, audio I/O, state, memory, observability. Embedding is deferred until the control plane is mature.
2. **Cancellation is structural, not bolted on.** Every long-running operation (LLM stream, TTS request, playback queue) carries a cancellation token bound to a turn ID. Barge-in cancels by turn ID across all subsystems.
3. **Backpressure is mandatory.** Every async boundary uses bounded queues with explicit overflow policy. No unbounded growth permitted anywhere on the critical path.
4. **Realtime audio path is special.** Audio callback never blocks, never allocates, never does I/O. SPSC ring buffer is the only mechanism crossing the audio thread boundary.
5. **Observability before features.** Structured logs, per-turn trace IDs, and metrics exist from Milestone 0. Every component emits events. No untraced control-plane action.
6. **Configuration is data, not code.** YAML schema + validation at startup; hot reload only for safe fields.
7. **Crash recovery is a first-class state machine, not error handling.** SQLite has explicit turn lifecycle (`in_progress`, `committed`, `interrupted`, `discarded`); orchestrator runs a recovery sweep on startup.

---

## 3. Pipeline

```
Mic → Resample → APM (AEC/NS/AGC) → VAD → Utterance Buffer → STT
   → Dialogue FSM ↔ Memory ↔ LLM
   → SentenceSplitter → TTS → Playback Queue → Resample → Speaker
                                              ↑
                                           Loopback (AEC reference)
```

The loopback path from final speaker output back into APM as the reference signal is the single most overlooked component in voice agent designs. It must be present before barge-in can work in speaker (non-headphone) mode.

---

## 4. Component Responsibilities

### 4.1 Audio I/O

- Backend: PortAudio (primary). RtAudio as fallback if PortAudio shows latency issues.
- Single `AudioDevice` abstraction; capture and playback share a `MonotonicAudioClock`.
- Sample-rate conversion at well-defined points (see §5).
- Audio callback runs on the OS-provided realtime thread; pushes to lock-free SPSC ring; never allocates.
- Underrun/overrun counters exported as metrics.

### 4.2 Audio Processing Module (APM)

- WebRTC APM for AEC, NS, AGC.
- Operates on 10 ms frames at 16 kHz mono (AEC standard).
- Receives **mic frame + reference frame (post-mix loopback)** in lockstep, time-aligned within the AEC tolerance window (~10 ms).
- Reference signal source is the *final* mixed speaker output, not the raw Piper output. This requires a loopback tap after the playback resampler.
- AEC delay estimation runs continuously; logs delay in metrics.

### 4.3 VAD

- Silero VAD primary, on cleaned (post-APM) audio.
- Optional WebRTC VAD as fast pre-gate to skip Silero on obvious silence.
- Endpointing logic:
  - Speech onset: probability > `onset_threshold` for `min_speech_ms`.
  - Speech end: probability < `offset_threshold` for `hangover_ms` (default 600 ms).
  - Minimum utterance length: 200 ms (drop shorter as noise).
- Emits `SpeechStarted`, `SpeechEnded` events on the event bus.

### 4.4 Utterance Buffer

- Owns the audio captured between `SpeechStarted` and `SpeechEnded` plus configurable pre/post padding (default 300 ms each side).
- Reference-counted `AudioSlice`: a single buffer is allocated per utterance; consumers (STT, optional disk recorder) hold shared_ptr handles.
- Slice is released when the last consumer is done.
- Bounded total in-flight capacity; oldest utterance dropped (with metric) on overflow.

### 4.5 STT Service

- whisper.cpp running as a separate process behind a small REST API.
- Utterance-based transcription for MVP; streaming partial transcription deferred.
- Input: 16 kHz mono PCM `AudioSlice`.
- Output: `FinalTranscript` event with text, confidence, audio duration, processing duration.
- Health: HTTP `/health`; supervisor pings every 5 s when idle.

### 4.6 LLM Service

- llama.cpp server with OpenAI-compatible API.
- Model: Qwen2.5-7B-Instruct, Q4_K_M default (Q5_K_M optional if VRAM allows after Whisper placement decision).
- Streaming via SSE.
- Keep-alive: orchestrator sends a 1-token completion every `keep_alive_interval_sec` (default 60 s) when idle to prevent model offload.
- HTTP client split:
  - `cpp-httplib` for health checks, simple completions, model warmup.
  - `libcurl` for SSE streaming (reliable, well-debugged, supports cancellation).

### 4.7 Dialogue Manager

Owns conversational state. Pure logic component; no audio, no I/O, no inference.

Responsibilities:
- Drive the Dialogue FSM (§6).
- Build LLM prompt from memory + recent turns (delegates to `PromptBuilder`).
- Receive LLM token stream; pass to `SentenceSplitter`.
- Forward sentences to TTS with sequence numbers tied to the active turn ID.
- Decide turn outcome on interruption.
- Decide what gets persisted to memory.
- Apply backpressure limits (max sentences, max queued TTS).

### 4.8 SentenceSplitter

Explicit, unit-tested component. Not "split on `.!?`".

- Streaming token-in, sentence-out.
- Handles abbreviations (`Dr.`, `e.g.`, `i.e.`, `etc.`), decimals (`3.14`), enumerations (`1.`, `2.`), code fences, ellipses.
- Emits a sentence only when:
  - Terminal punctuation seen, AND
  - Followed by whitespace + capital, OR end-of-stream, OR tokens-since-punctuation exceeds threshold (forces flush of long unterminated text).
- Configurable max-sentence length to force early flush (prevents TTS starvation on a very long sentence).

### 4.9 TTS Service

- Piper as a separate process. CPU-resident (frees VRAM).
- Sentence-level synthesis. Each request carries `(turn_id, sequence_number, text)`.
- Output: 22.05 kHz audio chunks tagged with turn_id and sequence_number.

### 4.10 Playback Queue

- Bounded queue of audio chunks tagged `(turn_id, sequence_number)`.
- Drop-policy on cancellation: when current turn ID is invalidated, drain queue immediately and emit a "playback interrupted" event.
- Stale chunks (chunks whose turn_id no longer matches the active turn) are rejected at enqueue and dequeue. This handles races between TTS completion and barge-in cancellation.

### 4.11 Memory Subsystem

- SQLite, WAL mode, `synchronous=NORMAL`.
- All writes happen on a dedicated memory thread; Dialogue Manager submits writes via a queue.
- Schema (see §9).
- Summarization is fully async: a background task computes summaries and updates a cached `current_summary` row; prompt assembly reads the cached version, never blocks waiting for fresh summary.

### 4.12 Supervisor

- Per-service state machine: `NotConfigured → Starting → Healthy → Degraded → Unhealthy → Restarting → Disabled`.
- Health probes per service.
- Exponential backoff with jitter on restart.
- Pipeline-level fail conditions (see §10).
- Keep-alive policy for LLM.

### 4.13 Event Bus

Two distinct mechanisms:

1. **Audio data path**: lock-free SPSC ring buffer between audio callback and audio processing thread. Fixed-size frames. No allocations in callback.
2. **Control event bus**: typed pub/sub for everything else. Bounded queues per subscriber with per-queue overflow policy. Built on a small in-process event dispatcher; not a generic message bus library.

### 4.14 Observability

- Structured JSON logs to stdout + rolling file.
- Per-turn trace IDs propagated through every event.
- Prometheus-compatible text metrics endpoint (HTTP), or a simpler stats dump on signal — TBD (see open questions).
- Trace stages per turn: `audio_segment, vad_end, stt_start, stt_final, llm_start, first_token, first_sentence, tts_start, first_audio, playback_start, playback_end, turn_committed`.

---

## 5. Audio Clock & Resampling Strategy

This was missing from both the original doc and the review.

- **Master clock**: capture device. Playback resamples to capture rate to keep AEC reference aligned.
- **Sample rate conversions**:
  - Mic native (likely 44.1 or 48 kHz) → 16 kHz mono for APM/VAD/STT pipeline. One resample at capture entry.
  - Piper output (22.05 kHz) → playback device rate at playback resampler. Loopback tap is taken **after** this resample (so AEC sees what the speaker actually emits).
- **MonotonicAudioClock**: a single clock object tied to capture frame counter, used to timestamp every audio frame and AEC reference frame. Drift between capture and playback hardware is corrected via the playback resampler ratio adjustment (slow drift) rather than buffer drops.
- Resampling library: libsamplerate (good quality, well-tested) or speex resampler (lighter, lower-quality acceptable for AEC-only paths). One choice for the whole project (see open questions).

---

## 6. Dialogue State Machine

```
Idle
  └─→ Listening (VAD armed)
        └─→ UserSpeaking (SpeechStarted)
              └─→ Transcribing (SpeechEnded → STT)
                    └─→ Thinking (FinalTranscript → LLM)
                          └─→ Speaking (first sentence ready → TTS → Playback)
                                ├─→ Completed (LLM done + queue drained)
                                └─→ Interrupted (UserInterrupted during Speaking)
                                      └─→ UserSpeaking (cancellation propagated)
```

**Assistant turn outcome states (orthogonal to FSM):**
`NotStarted, Generating, Speaking, Interrupted, Completed, Discarded`.

**Interruption persistence policy:**
- Interrupted before first complete sentence finished playing → `Discarded` (do not store).
- Interrupted after at least one complete spoken sentence → `Interrupted` (store completed-and-played text only, drop pending).

**Cancellation propagation on UserInterrupted:**
1. Bump active turn ID. All in-flight work tagged with old turn ID becomes stale.
2. Cancel LLM stream (HTTP request abort).
3. Cancel pending TTS requests (HTTP cancel or wait + discard).
4. Drain playback queue; emit `PlaybackInterrupted`.
5. Update assistant turn outcome state.
6. Transition FSM to `UserSpeaking`.

---

## 7. Threading & Executor Model

```
Audio capture callback thread (OS realtime)
  ↓ SPSC ring (frames)
Audio processing thread (APM + VAD + utterance assembly)
  ↓ event bus
STT I/O thread (HTTP to whisper.cpp)
  ↓ event bus
Dialogue thread (FSM, prompt assembly, sentence splitting)
  ↓ event bus
LLM I/O thread (SSE stream from llama.cpp)
  ↓ event bus
TTS I/O thread (HTTP to Piper)
  ↓ bounded queue
Playback thread (audio output callback)
Memory thread (SQLite writes, summarization)
Supervisor thread (health probes, restarts)
Metrics/logging thread (lossy)
```

Rules:
- Audio callback never blocks, never allocates, never does I/O.
- HTTP calls never on audio threads.
- Dialogue thread is single-threaded (FSM serialization).
- Memory thread is single-threaded (SQLite writer).
- Cancellation tokens propagate top-down via turn ID.

Concurrency primitive choice: **C++20 + asio (no Cobalt for MVP).** Cobalt + C++23 modules is a build-system risk we don't need to take. Revisit after Milestone 5.

---

## 8. Cancellation Model

Every operation that can outlive a turn carries a `TurnId` and a `CancellationToken`.

```cpp
struct CancellationToken {
    std::atomic<bool> cancelled{false};
    void cancel() noexcept;
    bool is_cancelled() const noexcept;
};

struct TurnContext {
    TurnId id;
    std::shared_ptr<CancellationToken> token;
    std::chrono::steady_clock::time_point started_at;
};
```

- `Dialogue` issues a new `TurnContext` at FSM transition into `Thinking`.
- `LlmClient::stream(turn, prompt) → cancellable iterator`.
- `TtsClient::synthesize(turn, sequence, text) → cancellable future<AudioChunk>`.
- `Playback::enqueue(turn, sequence, audio)` rejects if `turn != active_turn`.
- On `UserInterrupted`: `dialogue.bump_turn()` invalidates the token; all subsystems observing the token unwind cleanly.

Sequence numbers prevent the race "TTS request sent → user interrupts → TTS reply arrives late → audio plays anyway." The check is `(turn_id == active_turn) && (sequence > last_played_sequence)`.

---

## 9. Memory Architecture

### 9.1 Schema

```sql
CREATE TABLE sessions (
    id INTEGER PRIMARY KEY,
    started_at INTEGER NOT NULL,
    ended_at INTEGER,
    title TEXT
);

CREATE TABLE turns (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    role TEXT NOT NULL,                     -- 'user' | 'assistant'
    text TEXT,
    started_at INTEGER NOT NULL,
    ended_at INTEGER,
    status TEXT NOT NULL,                   -- in_progress | committed | interrupted | discarded
    interrupted_at_sentence INTEGER,
    audio_path TEXT                         -- optional, for user turns if recording enabled
);

CREATE TABLE summaries (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    range_start_turn INTEGER NOT NULL,
    range_end_turn INTEGER NOT NULL,
    summary TEXT NOT NULL,
    source_hash TEXT NOT NULL,              -- hash of source turn texts; allows re-summarize detection
    created_at INTEGER NOT NULL
);

CREATE TABLE facts (
    id INTEGER PRIMARY KEY,
    key TEXT NOT NULL,
    value TEXT NOT NULL,
    source_turn_id INTEGER REFERENCES turns(id),
    confidence REAL NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE(key)
);

CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX idx_turns_session ON turns(session_id, id);
CREATE INDEX idx_summaries_session ON summaries(session_id, range_end_turn);
```

### 9.2 Crash Recovery

On startup:
1. For every session with `ended_at IS NULL`: mark `ended_at = max(turns.ended_at)`.
2. For every turn with `status = 'in_progress'`: mark as `interrupted` with no `interrupted_at_sentence`.
3. Verify summary `source_hash` matches current source turns; flag stale summaries for async refresh.

### 9.3 Prompt Assembly

```
[system policy]
+ [user preferences from settings]
+ [durable facts: facts.key=value where confidence > threshold]
+ [cached session summary: most recent summary row]
+ [last N turns verbatim where status in ('committed', 'interrupted')]
+ [current user turn]
```

Last N: configurable, default 10. Summarization runs when turn count since last summary exceeds threshold (default 15).

### 9.4 Summarization Policy

- Runs on memory thread, never blocks Dialogue.
- Uses the same llama.cpp service but with a separate prompt template and lower max-tokens.
- Stores `source_hash` so we can detect when a summary should be regenerated from a new model or after correction.
- Facts extraction uses a stricter "only emit if directly stated" prompt to suppress hallucinated preferences.

---

## 10. Service Supervision

### Per-service health probes

- llama.cpp: HTTP `GET /health` every 5 s; on failure, lightweight `POST /v1/completions` with 1 token.
- whisper.cpp: HTTP `GET /health`; on failure, process liveness check.
- Piper: process liveness; periodic synthesize-test-phrase if `tts_health_synth_check: true`.

### Restart policy

```yaml
supervisor:
  restart_backoff_ms: [500, 1000, 2000, 5000, 10000]
  max_restarts_per_minute: 5
  llm_keep_alive_interval_sec: 60
  fail_pipeline_if_llm_down: true
  fail_pipeline_if_stt_down: true
  allow_tts_disabled: true
  tts_health_synth_check: false
```

Restart bounded by `max_restarts_per_minute`; on exhaustion the pipeline transitions to `Degraded` and emits a spoken error message if TTS is up.

---

## 11. Configuration System

### YAML schema enforced at startup

Validation produces structured errors with paths (`pipeline.nodes[2]: unknown node 'foo'`), not crashes.

### Hot-reloadable fields

- `llm.temperature`, `llm.max_tokens`, `llm.top_p`
- `dialogue.max_assistant_sentences`, `dialogue.max_tts_queue_sentences`
- `vad.onset_threshold`, `vad.offset_threshold`, `vad.hangover_ms`
- `tts.speed`
- `logging.level`

### Restart-required fields

- Audio device selection, sample rates
- Model paths, model names
- Service endpoints
- Pipeline graph topology
- Database path

Reload triggered by SIGHUP or a `/reload` HTTP endpoint (control plane).

---

## 12. Observability

### Logs

Structured JSON, one event per line. Required fields: `ts`, `level`, `component`, `event`, `turn_id` (when applicable). No multi-line stack traces in normal logs (optional file for debug).

### Metrics

Minimum (Prometheus exposition format on `/metrics`):

- `voice_turn_latency_ms` (histogram, labeled by stage: vad_end, stt_final, first_token, first_sentence, first_audio, playback_start)
- `voice_llm_first_token_ms` (histogram)
- `voice_llm_tokens_per_sec` (gauge)
- `voice_tts_first_audio_ms` (histogram)
- `voice_playback_underruns_total` (counter)
- `voice_vad_false_starts_total` (counter)
- `voice_interruptions_total` (counter)
- `voice_queue_depth` (gauge, labeled by queue name)
- `voice_aec_delay_estimate_ms` (gauge)
- `voice_service_restarts_total` (counter, labeled by service)

### Traces

Per-turn trace as a sequence of timestamped events sharing `turn_id`. Stored in logs; an offline tool can reconstruct waterfall view from the JSON log.

---

## 13. Latency Budget (Realistic)

P50 / P95 targets after Milestone 5:

| Stage                        | P50          | P95          |
|------------------------------|-------------:|-------------:|
| VAD end-of-turn delay        |   500 ms     |   800 ms     |
| STT final transcript         |   400 ms     |  1000 ms     |
| Prompt assembly              |    20 ms     |    80 ms     |
| LLM first token              |   600 ms     |  1500 ms     |
| First sentence ready         |  1000 ms     |  2200 ms     |
| TTS first audio              |   250 ms     |   600 ms     |
| Playback start               |    50 ms     |   150 ms     |
| **End-to-end (user-stop → first-audio)** | **~1.7 s** | **~3.5 s** |

The original "1–2 s" target is a P50 figure for short prompts. Long-context turns (after summary + 12 turns + complex query) regularly hit P95.

---

## 14. Testing Strategy

### Unit tests

- Dialogue FSM transitions (table-driven).
- SentenceSplitter (golden test set with abbreviations, lists, code, ellipses).
- PromptBuilder (snapshot tests).
- Memory write/recovery.
- Cancellation propagation (mock clients, assert no work after cancel).
- Config validation (invalid YAMLs produce specific errors).

### Component integration tests

- Fake STT → fake LLM → fake TTS pipeline driving the Dialogue FSM.
- Real llama.cpp smoke test against a tiny model (TinyLlama).
- Real whisper.cpp smoke test on fixed audio fixtures.
- Playback queue cancellation under simulated barge-in.

### Audio tests

- Recorded utterance fixtures with known boundaries (assert VAD endpoints within tolerance).
- Noise fixtures (white, pink, babble) with target false-start rate.
- Echo fixtures (TTS playing back through loopback) — assert AEC suppression > N dB.
- Long-session soak test: 4 hours scripted interaction; assert no crash, bounded memory, bounded queue depth, stable latency percentiles.

### Soak test acceptance

```
duration:        4 h
crashes:         0
heap growth:     < 50 MB after 1 h warmup
queue depth:     stable, no monotonic growth
latency P95:     within +20% of post-warmup baseline
service restarts: ≤ 2 (incidental), pipeline never enters failed state
```

---

## 15. Security & Privacy

- All processing local. No network calls outside localhost.
- Audio recordings: off by default. If enabled, written to a configurable path with session ID.
- "Wipe session" command: deletes session row and cascade.
- "Wipe all" command: drops and recreates schema, deletes audio files.
- Config option `redact_pii_in_logs: true` (default) suppresses transcript text in info-level logs.

---

## 16. Tech Stack (Final for MVP)

| Concern              | Choice                          | Rationale                                  |
|----------------------|---------------------------------|--------------------------------------------|
| Language             | C++20                           | Mature toolchain; defer C++23/Cobalt       |
| Build system         | CMake + presets                 | Standard                                   |
| Async                | asio (standalone or Boost)      | Battle-tested, no Cobalt risk              |
| HTTP simple          | cpp-httplib                     | Header-only, simple, sufficient            |
| HTTP SSE             | libcurl                         | Reliable streaming, well-debugged cancel   |
| JSON + YAML          | glaze                           | One library for both; reflective structs   |
| Audio                | PortAudio                       | Cross-platform, mature                     |
| APM                  | WebRTC APM (vendored)           | Industry standard AEC                      |
| Resampler            | libsamplerate                   | Quality                                    |
| VAD                  | Silero VAD (ONNX runtime)       | Best of class                              |
| DB                   | SQLite (WAL mode)               | Local, embeddable, atomic                  |
| Logging              | spdlog (with JSON sink)         | Mature, fast                               |
| Metrics              | prometheus-cpp                  | Standard format                            |
| Tests                | doctest or Catch2               | Header-only, fast                          |

---

## 17. Milestones (Adjusted Order)

The original order had AEC after barge-in, which is wrong. Without AEC the assistant's own voice triggers VAD and you spend a week debugging phantom interruptions.

### M0: Skeleton Runtime (1 week)
- C++ app starts, loads YAML config with validation.
- Event bus + bounded queues.
- Logging + metrics endpoints.
- Fake nodes wired into pipeline; FSM runs end-to-end on synthetic events.

### M1: LLM + Memory (1–2 weeks)
- llama.cpp integration with SSE streaming.
- PromptBuilder + SentenceSplitter (text-only, console I/O).
- SQLite schema + writes + crash recovery sweep.
- Async summarization stub.

### M2: Service Supervision (1 week)
- Per-service supervisor with state machine.
- Health probes for llama.cpp.
- Keep-alive policy.
- Restart backoff + circuit breaker.

(Done early because LLM crashes during long-context dev are inevitable; retrofitting supervision is painful.)

### M3: TTS + Playback (1–2 weeks)
- Piper integration.
- Playback queue with sequence-number cancellation.
- LLM → SentenceSplitter → TTS → Playback bridge.
- End-to-end text-input → spoken-output.

### M4: Audio Capture + VAD (1–2 weeks)
- PortAudio capture with SPSC ring.
- MonotonicAudioClock.
- Resampler.
- Silero VAD on cleaned audio.
- Utterance buffer with reference-counted slices.

### M5: STT (1 week)
- whisper.cpp integration.
- Utterance → transcript flow.
- End-to-end voice-input → text-input handled by M1.

### M6: AEC / NS / AGC (1–2 weeks)
- WebRTC APM integration.
- Loopback tap from playback resampler.
- Reference-signal alignment + delay estimation.
- Verify echo suppression on speaker mode.

### M7: Barge-In (1 week)
- Detect speech during Speaking state via VAD on AEC-cleaned audio.
- Cancellation propagation across LLM/TTS/Playback.
- Assistant-turn outcome handling (Discarded vs Interrupted).
- Memory persistence policy on interruption.

### M8: Production Hardening (2 weeks)
- Soak test infrastructure.
- Metrics dashboard (Grafana or simple HTML).
- Config hot-reload.
- Wipe/privacy commands.
- Packaging (single binary + config + service files).

**Total: ~12–14 weeks** for a single competent C++ developer to MVP.

---

## 18. Risk Register

| # | Risk                                                          | Severity | Mitigation                                                           |
|---|---------------------------------------------------------------|---------:|----------------------------------------------------------------------|
| 1 | AEC reference signal misrouted → barge-in unreliable          | High     | Tap loopback after playback resample; AEC delay estimation + metric  |
| 2 | Audio clock drift over multi-hour session                     | Medium   | MonotonicAudioClock + slow resampler-ratio adjustment                |
| 3 | LLM first-token latency at deep context                       | Medium   | Cap context size; keep-alive; consider Q4_K_S                        |
| 4 | TTS queue overflow during long LLM bursts                     | Medium   | `max_tts_queue_sentences` hard cap; LLM stop on overflow             |
| 5 | Cancellation race: stale TTS audio plays after barge-in       | High     | Turn ID + sequence number on every chunk; reject at enqueue/dequeue  |
| 6 | SQLite blocks on memory writes                                | Low      | WAL mode + dedicated memory thread + write queue                     |
| 7 | Memory summary drift over multi-hour sessions                 | Medium   | Source-hash on summaries; strict facts-extraction prompt             |
| 8 | VAD false-positive from TTS echo (speaker mode, no AEC)       | High     | AEC before barge-in (M6 before M7); document headphone-only fallback |
| 9 | llama.cpp idle offload causes cold-start regression           | Medium   | Periodic keep-alive completion every 60 s                            |
|10 | Build system over-reach (Cobalt + C++23 modules)              | Medium   | C++20 + asio for MVP; revisit later                                  |
|11 | Crash mid-turn leaves SQLite in inconsistent state            | Medium   | Turn lifecycle + startup recovery sweep                              |
|12 | GPU memory exhaustion on 8 GB card with Whisper + Qwen        | Medium   | Whisper on CPU by default; Qwen Q4_K_M; budget verified at startup   |
|13 | SentenceSplitter mis-fires on abbreviations/lists/code        | Medium   | Dedicated component with golden test corpus                          |
|14 | Resampler chain introduces phase issues for AEC               | Low      | One resampler library (libsamplerate); document conversion points    |

---

## 19. Success Criteria for MVP

1. End-to-end P50 latency ≤ 2 s, P95 ≤ 3.5 s on the target hardware.
2. 4-hour soak test passes acceptance criteria (§14).
3. Barge-in reliability: ≥ 95 % correct cancellation within 300 ms of detected user speech, in headphone mode; ≥ 80 % in speaker mode with AEC.
4. No crashes in soak test.
5. Crash mid-session: orchestrator restarts cleanly, recovers session state, continues conversation.
6. Memory growth bounded over 4 hours.
7. Config errors at startup produce clear, path-qualified messages.

---

## 20. Key Architectural Decision (Restated)

> The orchestrator owns timing, cancellation, state, memory, and observability.
> Model runtimes live in separate processes until the control plane is mature.
> Realtime audio paths are isolated from everything else.
> Cancellation, backpressure, and recovery are first-class state machines, not error handling.

Everything else in this document is a consequence of those four sentences.
