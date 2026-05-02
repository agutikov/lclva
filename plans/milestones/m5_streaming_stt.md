# M5 — Streaming STT + Speculation

> **Update:** the engine-choice decision matrix below (Step 1, options A/B/C)
> was **resolved before M5 starts** by inserting **M4B — Voice-Backend
> Consolidation onto Speaches** between M4 and M5. M4B brings up Speaches,
> ships a request/response STT client, and migrates the M3 TTS client to
> the same Speaches base URL. M5 therefore picks up with **option B
> already in production** and focuses on swapping the request/response
> STT client for Speaches' streaming endpoint plus the
> Dialogue-Manager-side speculation work. See
> `plans/milestones/m4b_speaches_consolidation.md` and `open_questions.md` L1.
> Step 1 below is preserved for historical context but no longer drives
> the milestone.
>
> **Streaming protocol correction (M4B Step 0 finding):** Speaches
> exposes the streaming endpoint at `POST /v1/realtime` and the
> implementation is **WebRTC** (`Realtime Webrtc` in the OpenAPI
> spec), **not WebSocket** as Step 1 below speculated.
>
> **Spike resolved 2026-05-02 — `libdatachannel` is the M5 transport.**
> A live SDP offer/answer round-trip against Speaches' aiortc with
> `libdatachannel 0.24.1` (Manjaro `extra` repo, MPL-2.0, CMake target
> `LibDataChannel::LibDataChannel`) negotiates cleanly: ICE+DTLS reaches
> Connected, the `oai-events` data channel opens, and Speaches
> proactively pushes `session.created`. Pinned by
> `tests/test_speaches_realtime_smoke.cpp` (gated on `ACVA_HAVE_LIBDATACHANNEL`
> and Speaches `/health`). No fallback proxy is needed.
>
> **Wire-level surprises pinned by the spike** (load-bearing for Step 2):
>
> 1. **POST `/v1/realtime?model=<id>` body is the SDP offer text**
>    (Content-Type `application/sdp`); the response 200 body is the
>    SDP answer. The OpenAPI schema does NOT document this — body is
>    declared as `null` and only the `model` query param is captured.
>    Don't trust the schema; trust the wire.
> 2. **Speaches wraps every realtime event in its own envelope** before
>    putting it on the data channel:
>    ```json
>    {"id":"event_…","type":"partial_message",
>     "data":"<base64-of-inner-json>",
>     "fragment_index":N,"total_fragments":M}
>    ```
>    The M5 STT client must reassemble fragments by `id`, base64-decode
>    `data`, then parse the inner OpenAI Realtime event payload.
> 3. **The query-string `model` parameter sets the session model, not
>    the transcription model.** The default `session.created` arrives
>    with `input_audio_transcription.model =
>    "Systran/faster-distil-whisper-small.en"` regardless of the query
>    string. To pin transcription to
>    `deepdml/faster-whisper-large-v3-turbo-ct2`, the client must send
>    a `session.update` event over the data channel after open.
> 4. **Server-side VAD is on by default** (`turn_detection.type =
>    "server_vad"`, threshold 0.9, silence 550 ms). M5 needs to decide
>    whether to keep server VAD or send `session.update` with
>    `turn_detection: null` and rely on our Silero VAD from M4. Open
>    question — track in `open_questions.md` section L.
> 5. **Default modalities include `audio` output and a Kokoro TTS
>    voice.** STT-only consumers must `session.update` with
>    `modalities: ["text"]` (and `tools: []`) to silence the TTS path.

**Estimate:** 1.5–2 weeks (post-M4B). Was 1.5–3 weeks under the
pre-M4B plan; the lower end of that range now applies because engine
selection + request/response wiring already shipped.

**Depends on:** M1 (Dialogue Manager wiring; LLM client cancellation),
M4 (utterance pipeline; AudioSlice), **M4B (Speaches container,
OpenAI-compatible STT/TTS clients, real `FinalTranscript` flowing on
the bus)**.

**Blocks:** M7 (full barge-in needs partials, *if* we keep speculation in MVP). If speculation is deferred (Option C), M7 still ships — just with slightly higher perceived latency.

## Goal

Three things land together:

1. **Streaming partial transcription**: the STT backend produces `PartialTranscript` events as the user speaks; a `FinalTranscript` closes the utterance.
2. **Multilingual**: language is detected per utterance; the language flows through prompt assembly, TTS voice selection, and memory `lang` columns.
3. **Speculative LLM start**: the Dialogue Manager opportunistically begins LLM generation against a stable prefix before the final transcript arrives, then either keeps or restarts based on reconciliation.

## Out of scope

- Production-quality multilingual evaluation across all Whisper-supported languages. M5 verifies a small set (en, ru, de, es, fr) on Common Voice fixtures.

---

## Step 1 — Choose the streaming STT engine (decision point)

Upstream `whisper.cpp/examples/server` (used through M4 in Compose) is **request/response only**. To stream partials we have three options. The decision is logged as an open question in `plans/open_questions.md`; pick at the start of M5.

### Option A — Custom C++ wrapper around whisper.cpp's `stream` example

What we originally planned.

- New deps: `whisper.cpp` upstream, linked into our own binary.
- Files in `packaging/whisper-server/`: `CMakeLists.txt`, `main.cpp` (cpp-httplib server), `whisper_session.hpp/cpp`, `README.md`. Independent CMake subproject.
- HTTP contract:
  - `POST /utterance/start` `{utterance_id, expect_lang}` → `{ok}`
  - `POST /utterance/audio` (raw PCM, 16 kHz mono int16, `X-Utterance-Id` header) → 204
  - `GET /utterance/stream?id=...` → SSE: `partial` then `final` events
  - `POST /utterance/end` → `{ok}`
- Streaming algorithm: sliding window with overlap, per whisper.cpp's `examples/stream`. Run inference every `step_ms` (default 500) on the last `keep_ms + audio_since_last_step`; emit a partial after each step; stable prefix = unchanged substring.
- **Pros:** maximal control; uses whisper.cpp directly (matches model files we already have); zero upstream dependency drift.
- **Cons:** ~5 days of work; one more subproject to maintain; we own the streaming algorithm.

### Option B — Adopt Speaches (faster-whisper) [RECOMMENDED]

Drop-in OpenAI-compatible Realtime / streaming transcription server. Backend is `faster-whisper` (CTranslate2), typically faster than whisper.cpp on CPU. Active project, growing ecosystem.

- New deps: none in our codebase. Add a `speaches` service to Compose.
- The orchestrator's `WhisperClient` talks an OpenAI-Realtime-style WebSocket / SSE protocol.
- Compose:
  ```yaml
  speaches:
    image: ghcr.io/speaches-ai/speaches:latest-cpu   # or :latest-cuda
    ports: ["127.0.0.1:8082:8000"]
    volumes:
      - ${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}/speaches:/data
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8000/health"]
  ```
- Models: faster-whisper uses CTranslate2 format. Speaches downloads what it needs on first use.
- **Pros:** ~5 days saved; faster STT engine; battle-tested by other projects; we get OpenAI-compat for both ends of the pipeline (consistent with llama-server).
- **Cons:** upstream dependency we don't control; different model file format from M4's whisper.cpp setup (we'd swap the M1.B compose `whisper` service for `speaches`); WebSocket protocol on the client side instead of plain SSE.

### Option C — Defer speculation past MVP

Use upstream `whisper.cpp:server` (request/response) all the way through MVP. Drop the `SpeculativeThinking` concurrent state from the FSM. End-to-end latency takes a 300–500 ms hit on the median turn — perceived but bearable.

- New deps: none.
- Files: `src/stt/whisper_client.{hpp,cpp}` only — no streaming, no speculation logic in dialogue manager.
- M5 collapses to: multilingual flow + simple non-streaming whisper integration. ~5–6 days total.
- **Pros:** smallest scope; ships fastest; lowest risk.
- **Cons:** worse latency P95; the speculation FSM logic is post-MVP work.

### Decision matrix

| Option | Time | Streaming partials | Engine | Risk | Lock-in |
|---|---|---|---|---|---|
| A — Custom wrapper | ~3 weeks | Yes | whisper.cpp | Medium (we own a subproject) | Low |
| B — Speaches | ~1.5 weeks | Yes | faster-whisper | Low (upstream maintained) | Medium |
| C — Defer | ~1 week | No | whisper.cpp | Low | None |

**Recommendation:** Option B unless Speaches has visibly stagnated by the time M5 starts. Verify before committing: read recent commit activity, check the model formats supported, run a 5-minute smoke against their image.

The text below assumes Option A or B (streaming exists). If Option C is chosen, skip the speculation policy section and the FSM concurrent-state changes; M5 reduces to "wire whisper-server through with multilingual support."

## Step 2 — Streaming STT client

> **Architecture revised post-spike (2026-05-02).** The original sketch
> below modeled per-utterance backend connections (one SSE stream per
> utterance). The realtime spike pinned the actual contract: Speaches
> `/v1/realtime` is a **long-lived WebRTC session** in which utterances
> are delineated by `input_audio_buffer.commit` events on a single
> shared data channel. Re-negotiating SDP per utterance would burn
> ~500 ms per turn for no gain. See `open_questions.md` L4.

The client is split into two source-level pieces and lands in two
sub-slices.

### Step 2.a — Session lifecycle (✅ landed 2026-05-02)

**Files:**
- `src/stt/realtime_envelope.{hpp,cpp}` — Speaches' `partial_message`
  / `full_message` envelope reassembler + base64 decoder. Pure logic,
  no network, 9 unit tests in `tests/test_realtime_envelope.cpp`.
- `src/stt/realtime_stt_client.{hpp,cpp}` — `RealtimeSttClient` owns
  one long-lived `rtc::PeerConnection`. `start(timeout)` blocks
  through ICE+SDP+DTLS+`session.updated`, returning true when the
  state reaches `Ready`. `stop()` is idempotent and safe from any
  thread. Compile-time gated on `ACVA_HAVE_LIBDATACHANNEL`; the stub
  fallback returns false from `start()`.
- Live integration in `tests/test_speaches_realtime_smoke.cpp`:
  asserts the client reaches `Ready` within 15 s against the Compose
  Speaches container.

States: `Idle → Connecting → Configuring → Ready` (success path);
any error transitions to `Failed`; `stop()` transitions to `Closed`.

`session.update` payload sent on bring-up: `modalities: ["text"]`,
`tools: []`, `input_audio_transcription.model: <cfg.stt.model>`. The
`turn_detection` field is **deliberately omitted** because Speaches'
`PartialSession` schema rejects `null` and has no "disable" variant
(see `open_questions.md` L5).

### Step 2.b — Per-utterance audio + transcripts (✅ landed 2026-05-02)

**Audio transport: data channel, not RTP.** The original sketch
called for Opus + RTP on the WebRTC audio track. We discovered
during implementation that Speaches accepts a parallel
`input_audio_buffer.append` path on the data channel (see
`realtime/input_audio_buffer_event_router.py` line 124), in which
the audio is base64-encoded raw 24 kHz int16 mono PCM. Both routes
funnel into the same internal pubsub. We use the data-channel
route — it skips libopus, RTP packetization, and the real-time
pacer entirely. Net savings: ~3-4 days of implementation, one
fewer C++ dep.

**Files:**
- `src/stt/realtime_event_dispatch.{hpp,cpp}` — typed dispatcher
  for OpenAI-Realtime events (delta / completed / committed /
  session.updated / error). 11 unit tests in
  `tests/test_realtime_event_dispatch.cpp`.
- `src/stt/realtime_envelope.{hpp,cpp}` extended with
  `base64_encode`, `build_input_audio_buffer_append_json`,
  `build_simple_event_json`. Outgoing direction is unwrapped
  (no fragmentation envelope — server expects raw client-event JSON).
- `src/stt/realtime_stt_client.{hpp,cpp}` extended with the
  per-utterance API and a `clear_active_locked()` helper.

**API:**
```cpp
void begin_utterance(dialogue::TurnId turn,
                     std::shared_ptr<dialogue::CancellationToken> cancel,
                     UtteranceCallbacks cb);
void push_audio(std::span<const std::int16_t> samples_16k);
void end_utterance();
```
- `begin_utterance` sends `input_audio_buffer.clear` to discard any
  residual audio from a prior turn.
- `push_audio` resamples 16 kHz → 24 kHz via the existing
  `audio::Resampler` (soxr High quality), base64-encodes, sends
  one `input_audio_buffer.append` per chunk on `oai-events`.
- `end_utterance` flushes the resampler tail and **does NOT** send
  `input_audio_buffer.commit` — see open question L5. Callers
  must arrange a trailing silence window (M4 Silero hangover does
  this in production; the test pads explicit silence). Server VAD
  detects speech-stop and auto-commits, the resulting transcript
  fires on the user's `on_final` callback.

**Events parsed:**
- `conversation.item.input_audio_transcription.delta` →
  `event::PartialTranscript` with running concatenation of deltas.
- `conversation.item.input_audio_transcription.completed` →
  `event::FinalTranscript` (turn id, text, lang).
- `input_audio_buffer.committed` → records the server-assigned
  `item_id` so subsequent transcription events route to the
  correct turn.
- `error` → user `on_error` callback.

**I/O threading:** libdatachannel `onMessage` callbacks run on its
internal thread and serialize per channel. `RealtimeSttClient` uses
one mutex + cv for state transitions. User callbacks are invoked
*outside* the lock so they can re-enter the client safely. Per-utterance
state (`active_turn`, `active_item_id`, `partial_text`, resampler) is
guarded by the same mutex.

**Live integration test:**
`tests/test_speaches_realtime_smoke.cpp::"RealtimeSttClient: WAV
fixture round-trip"` synthesizes a known phrase via Speaches TTS,
pushes 200 ms chunks (matching the M4 production cadence), pads
800 ms of trailing silence, and asserts the model-stable suffix
("smoke test") arrives in `on_final`. 10/10 stable on the dev
workstation.

## Step 3 — Wire VAD → Whisper (✅ landed 2026-05-02)

**Pipeline live-audio sink.** `AudioPipeline` gained a
`set_live_audio_sink(LiveAudioSink)` setter. Inside `process_frame`,
the resampled 16 kHz chunk is pushed through the sink whenever the
endpointer's `in_speech_` flag is true (between `SpeechStarted` and
`SpeechEnded` outcomes, including the trigger frames at both
boundaries). Sink calls coexist with the M4B `UtteranceBuffer`
appends — both STT paths can run, in practice main.cpp picks one.

**main.cpp dispatch:** when `cfg.stt.streaming && cfg.stt.base_url
&& cfg.audio.capture_enabled`, the orchestrator constructs a
`RealtimeSttClient`, calls `start(15s)` synchronously at startup,
wires:

- `audio_pipeline->set_live_audio_sink([](samples) { realtime_stt->push_audio(samples); })`
- `bus.subscribe<SpeechStarted>` → `realtime_stt->begin_utterance(NoTurn, fresh cancel, callbacks)`
- `bus.subscribe<SpeechEnded>` → `realtime_stt->end_utterance()`

The `UtteranceCallbacks` registered per-utterance publish
`PartialTranscript` / `FinalTranscript` straight onto the bus; the
existing dialogue Manager subscription consumes them transparently —
no FSM or Manager changes required.

**Path selection:**

| `stt.base_url` | `stt.streaming` | `audio.capture_enabled` | Path |
|---|---|---|---|
| empty | — | — | STT disabled |
| set | true | true | M5 streaming (RealtimeSttClient + live sink) |
| set | true | false | STT disabled (no audio source to stream) |
| set | false | — | M4B request/response (UtteranceReady → OpenAiSttClient) |

**No FSM changes** — `SpeechStarted` / `SpeechEnded` events drive the
existing FSM transitions; the only new thing is that they ALSO drive
the realtime client lifecycle in parallel.

**Tests added:**
- `tests/test_audio_pipeline.cpp::"AudioPipeline: live-audio sink fires
  only inside the utterance window"` — synthetic VAD probability +
  `pump_for_test`, asserts sink stays silent during pre-speech and
  post-hangover phases, fires for every chunk in the utterance window.

**Acceptance gate (manual):** flip `cfg.audio.capture_enabled: true`,
flip `cfg.audio.half_duplex_while_speaking: true` (since we don't
have AEC yet), run `./_build/dev/acva` with the compose stack up,
speak into the mic — full mic→STT→LLM→TTS→speakers loop closes.

### Step 3.b — Half-duplex mic gate (✅ landed 2026-05-02)

Speakers-without-AEC fallback so the agent is usable on speakers
before M6 lands. When `cfg.audio.half_duplex_while_speaking` is true
(default false), the FSM's state observer toggles a lock-free
`HalfDuplexGate` whenever it enters/leaves `Speaking`; the
`CaptureEngine::on_input` callback consults the gate and silently
drops mic samples while it's active (plus a `half_duplex_hangover_ms`
window after `Speaking` ends, default 200 ms, to absorb speaker tail
and room reverb).

**Trade-off:** no barge-in. The user can't interrupt the assistant
because the mic isn't being listened to. This is the explicit
alternative to M6 + M7 — both supersede this gate once they land
(M6 keeps mic open via AEC; M7 wires barge-in proper). Defaults off
because the project's stated UX is full-duplex.

**Files:**
- `src/audio/half_duplex_gate.hpp` — header-only template class on
  `Clock` (defaults to `std::chrono::steady_clock`); `set_speaking`
  + `should_drop_now`, both lock-free. 8 unit tests with an injected
  manual clock in `tests/test_half_duplex_gate.cpp`.
- `src/audio/capture.{hpp,cpp}` — `CaptureEngine::set_half_duplex_gate`
  + `frames_gated()` counter; gate consulted in the realtime
  `on_input` path.
- `src/dialogue/fsm.{hpp,cpp}` — `Fsm::set_state_observer(prev, next)`
  hook (mirrors the existing `set_turn_outcome_observer`).
- `src/main.cpp` — when `capture_enabled && half_duplex_while_speaking`,
  constructs the gate, wires it into both the FSM observer and the
  capture engine.
- `config/default.yaml` — documents the two new fields.

## Steps 4 & 5 — moved to M9

**Originally:** add `SpeculativeThinking` FSM state + a speculation
gate that opportunistically starts the LLM against a stable partial
prefix, then reconciles when the final transcript arrives.

**Status: deferred to [`m9_speculation.md`](m9_speculation.md).**

**Why:** speculation requires a partial-transcript source.
Speaches' realtime endpoint as of 2026-05-02 does NOT emit
`conversation.item.input_audio_transcription.delta` events — its
transcriber (`realtime/input_audio_buffer.py`) awaits the full WAV
via `transcription_client.create(...)` after `commit` and publishes
a single `transcription.completed` event. M5's chosen Option B
backend therefore can't drive speculation. Rather than block M5
closure on a multi-day Speaches PR with uncertain merge timing — or
retroactively switch to Option A (custom whisper.cpp wrapper, a
multi-week scope creep at this point in M5 implementation) — we
lifted speculation work into M9. M5 ships without it; M9 picks it up
when partials become available.

The acceptance gates that depended on partials moved with it (see
the M9 plan).

## Step 6 — Multilingual flow (✅ landed 2026-05-02)

**Configured-language baseline** rather than per-utterance detection.
Speaches' realtime endpoint doesn't return detected language (the
OpenAI Realtime spec it implements has no `language` field on
`transcription.completed`); per-utterance detection lands in M9
along with streaming partials.

**Wiring:**
- New `cfg.stt.language` (BCP-47, default `"en"`).
- `RealtimeSttClient` sends it as `input_audio_transcription.language`
  in `session.update`, so Whisper transcribes against the configured
  language rather than auto-detecting.
- `RealtimeSttClient` stamps the same value onto `FinalTranscript.lang`
  for every transcript it emits — non-empty `lang` propagates through:
  - `Manager::run_one` → `PromptBuilder::build({lang, ...})` → picks
    `cfg.dialogue.system_prompts[lang]` (with English fallback).
  - `TtsBridge` → routes to `cfg.tts.voices[lang]`.
  - `TurnWriter` → writes `lang` into the SQLite `turns.lang` column.
- M4B `OpenAiSttClient` path (still alive for fixture demos) gets
  the same `cfg.stt.language` via `SttRequest.lang_hint`.

**Default English path** (out-of-box `acva` run) is unchanged in
behavior: cfg.stt.language defaults to "en" → English voice +
English system prompt + lang="en" in memory.

**To switch to Russian:** set `cfg.stt.language: "ru"`. The
downloader (`scripts/download-speaches-models.sh`) pulls all four
upstream `piper-ru_RU-{denis,dmitri,irina,ruslan}-medium` voices;
`config/default.yaml` ships `ruslan` as the active `ru` voice.
`config/default.yaml` also ships a Russian system_prompts entry.

**M9 will replace this** with detected-per-utterance language once a
streaming-partial-emitting STT backend is in place.

### Original Step 6 plan (kept for context)

Touch points:
- `FinalTranscript.lang` populated from Whisper's detection.
- Memory schema gains the `lang` column (already in §9.1; M1 wires writes for `en`; M5 wires for arbitrary langs).
- `PromptBuilder` consumes `lang` and selects the system prompt from `cfg.dialogue.system_prompts[lang]` (with English fallback).
- `PiperClient` already routes by `lang` (M3).
- `Manager` pulls the assistant turn's `lang` from the user turn's `FinalTranscript.lang` when it submits to LLM and TTS.

**New event field validation:** if `FinalTranscript.lang` is empty (e.g., low confidence detection), fall back to `cfg.dialogue.fallback_language` (default `en`).

## Step 7 — Tests

| Test | Scope |
|---|---|
| Whisper wrapper unit | `packaging/whisper-server/tests/` — golden 5-second clip, expect specific partials and final |
| `test_whisper_client.cpp` | fake server emits known SSE; assert callbacks fire correctly |
| `test_speculation.cpp` | gate fires under stable conditions, rejects under instability; reconcile match/mismatch logic |
| `test_fsm.cpp` (extends M0) | new transitions: speculation start/keep/restart |
| Multilingual smoke (gated) | `ACVA_REAL_WHISPER=1` against Common Voice clips in en/ru/de/es/fr |
| End-to-end | speak in English; observe partials → final → speculation kept; speak with mid-sentence revision; observe restart |

## Demo commands (planned)

Two `acva demo <name>` subcommands cover M5 — one mic-driven, one fixture-driven.

### `acva demo stt` — mic → transcript

Records 5 seconds of mic input, pipes it through the chosen Whisper
backend (Option A/B from §1), and prints partials + final transcript.

Expected output (Option A or B):

```
demo[stt] backend='speaches' lang=auto duration=5s
demo[stt] speak now…
  partial  (seq=0, p=0.42, stable=12)  the q
  partial  (seq=1, p=0.61, stable=18)  the quick brown
  partial  (seq=2, p=0.79, stable=24)  the quick brown fox
  final    (lang=en, conf=0.91)        The quick brown fox jumps.
demo[stt] done: partials=4 final_chars=27 detected_lang=en
```

Under the M9-deferred path (Speaches without streaming partials):
no `partial` lines, only the `final` transcript. Once M9 lands a
partial-transcript source, the partial lines reappear without
demo changes.

### `acva demo stt --fixture FILE.wav` — offline transcription

Pipes a 16 kHz mono WAV through the same Whisper client without
involving the mic. Useful in CI / soak runs where speaking isn't an
option, and for regression-checking accuracy on a known clip.

Failure modes (both):
- `whisper /health probe failed` → the Whisper container isn't up; `acva demo health` to confirm.
- `final_chars=0` → no speech detected; check VAD threshold (`acva demo capture` is the right tool to debug that).
- `detected_lang=` mismatch — the model's language detector got confused; pin it via `cfg.stt.language` if needed.

What neither covers: the speculation-reconcile logic in the dialogue
manager — that's exercised end-to-end by `acva demo chat` once M5 is
landed and the chat demo is updated to publish via the real STT path.

## Acceptance

1. With Speaches running in Compose, speaking a sentence emits one
   `FinalTranscript` on the bus. (Partials are deferred to M9 — see
   "Steps 4 & 5 — moved to M9" above.)
2. Multilingual: speaking in a configured non-English language routes
   to the matching Piper voice (M3) and the LLM is prompted in that
   language.
3. Memory rows have populated `lang` columns matching the configured
   per-utterance language.
4. End-to-end speech-to-speech works: speaking into the mic produces
   a spoken reply through the speakers via the
   capture → VAD → STT → LLM → TTS → playback path. Half-duplex mic
   gate prevents the assistant's own voice from triggering VAD until
   M6 AEC lands.

**Speculation-related gates** (median first-token-ready 300 ms savings,
mid-utterance revision, `voice_speculation_*` counters) moved to M9
along with the implementation.

## Risks specific to M5

| Risk | Mitigation |
|---|---|
| Speaches upstream drift | Pin image digest; track upstream commit; one-line update PRs |
| Wrong language detection causes voice/prompt mismatch | Confidence threshold; fallback language; metric `voice_lang_low_confidence_total` |
| Multilingual STT slow on CPU | Configurable model size; benchmark at startup |

## Time breakdown (after M9 split)

Steps 4 & 5 moved to M9. Remaining M5 work, all under Option B
(Speaches):

| Step | Cost |
|---|---:|
| 1 Streaming engine setup | done in M4B (0.5 d carryover) |
| 2.a Session lifecycle | ~1 d (✅ landed) |
| 2.b Per-utterance audio + transcripts | ~1 d (✅ landed) |
| 3   VAD → STT wiring + half-duplex gate | ~1 d (✅ landed) |
| 6   Multilingual flow (configured-language baseline) | ~1 d |
| 7   Tests + acceptance + cleanup TODO | ~1.5 d |
| **Remaining to close M5** | **~2.5 d** |

## TODOs / known issues to clean up before closing M5

- ~~Flaky bus-timing tests~~ ✅ fixed 2026-05-02. Three pre-existing
  flakes addressed:
  - `AudioPipeline: forced VAD probability...` — wait predicate
    exited on `speech_started || utterance_ready` going non-zero,
    racing the `speech_ended` assertion. Fixed by waiting for all
    three counters.
  - `ServiceMonitor: Unhealthy → Healthy on a single OK probe`
    + `ServiceMonitor: probe-fn exception is recorded as a failure` —
    `mon.state()` raced the next probe at `probe_interval_degraded=2ms`.
    Fixed by reading state via the `HealthSink` bus subscriber, which
    captures the deterministic transition history.
  - `PlaybackEngine: render_into is callable directly`
    + `PlaybackEngine: prefill_ms=0 disables the gate`
    + `PlaybackEngine: prefill threshold` — `force_headless(60s)` still
    spawned the headless ticker, which got one `render_into` call in
    before its first sleep, racing the test's manual call. Fixed by
    treating `force_headless(0ms)` as "no ticker, manual mode" — the
    publisher thread still runs (so PlaybackFinished events still
    flow) but no other thread touches `render_into`.
