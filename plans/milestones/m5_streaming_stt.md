# M5 — Streaming STT + Speculation

**Estimate:** 1.5–3 weeks. The range depends on the streaming-engine decision in Step 1 below.

**Depends on:** M1 (Dialogue Manager wiring; LLM client cancellation), M4 (utterance pipeline; AudioSlice).

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

## Step 2 — Whisper client

**Files:**
- `src/stt/whisper_client.hpp`
- `src/stt/whisper_client.cpp`

```cpp
struct UtteranceSession {
    std::string utterance_id;
    dialogue::TurnId turn;
    std::shared_ptr<dialogue::CancellationToken> cancel;
};

struct UtteranceCallbacks {
    std::function<void(event::PartialTranscript)> on_partial;
    std::function<void(event::FinalTranscript)>   on_final;
    std::function<void(std::string err)>          on_error;
};

class WhisperClient {
public:
    WhisperClient(const Config& cfg);
    ~WhisperClient();

    // Begin a new utterance. Spins up an SSE consumer for the partial stream.
    void begin(UtteranceSession session, UtteranceCallbacks cb);

    // Push a chunk of 16 kHz mono PCM. Async — returns immediately.
    void push_audio(std::string_view utterance_id, std::span<const std::int16_t> samples);

    // Signal end-of-utterance. Server emits final event soon after.
    void end(std::string_view utterance_id);

    bool probe();
};
```

I/O thread architecture: one libcurl easy-handle per active utterance for the SSE stream; `push_audio` posts via cpp-httplib. Reentrant — multiple utterances can be in flight (rare but possible during barge-in races).

## Step 3 — Wire VAD → Whisper

In M4 we got `UtteranceReady` with an `AudioSlice` after `SpeechEnded`. For streaming, we need to push audio **as it arrives**, not at the end.

**Change:** the audio-processing thread, in addition to assembling the utterance buffer, streams chunks directly to the WhisperClient between `SpeechStarted` and `SpeechEnded`. The full `AudioSlice` is still kept around for re-transcription on cancellation/restart.

**Sequence:**
- `SpeechStarted` → mint utterance id; FSM in `UserSpeaking`; `WhisperClient::begin`.
- Each 32-ms VAD chunk → `WhisperClient::push_audio` (also appended to UtteranceBuffer for completeness).
- `SpeechEnded` → `WhisperClient::end`; FSM transitions to `Transcribing` to wait for the `final` event.
- Server emits `partial` events asynchronously throughout — they arrive on the bus as `PartialTranscript`.
- Server emits `final` event → `FinalTranscript` on bus.

## Step 4 — FSM gains `SpeculativeThinking`

M0 deferred this. Now we add a concurrent sub-state.

**Files:** modify `src/dialogue/fsm.hpp/cpp`.

**New behavior:** the FSM listens to `PartialTranscript`. When the speculation policy fires (Step 5), the FSM enters `SpeculativeThinking` *while still being in* `UserSpeaking` or `Transcribing`. The Dialogue Manager submits an LLM request against the speculative prompt; the FSM tracks the speculative turn id separately from the active turn id.

```cpp
struct FsmSnapshot {
    State state;
    TurnId active_turn;
    TurnOutcome outcome;
    bool speculative_in_flight;            // NEW
    TurnId speculative_turn;               // NEW: same as active or distinct
    std::uint32_t sentences_played;
    // ...
};
```

When `FinalTranscript` arrives:
- If the speculation matches: keep it; transition to `Speaking` as soon as the first sentence is ready (i.e., as before).
- If it diverges: cancel the speculative LLM (turn-id bump just for the speculative subtask), submit a fresh LLM request against the final transcript.

Both paths are normal cancellation. The `TurnContext` machinery from M0 already supports this; we just allocate two of them.

## Step 5 — Speculation policy

**File:** `src/dialogue/speculation.hpp`, `src/dialogue/speculation.cpp`.

```cpp
struct SpeculationConfig {
    std::chrono::milliseconds hangover_ms{250};       // shorter than VAD's 600
    std::size_t min_chars = 20;
    std::chrono::milliseconds stability_ms{200};
    double match_ratio = 0.9;
    int max_restarts_per_minute = 8;
};

class SpeculationGate {
public:
    explicit SpeculationGate(SpeculationConfig cfg);

    // Called on each PartialTranscript and on VAD probability updates.
    // Returns true once when conditions are met to start a speculative LLM run.
    bool should_speculate(const PartialState& state);

    // Called when FinalTranscript arrives. Returns Match (keep), Mismatch
    // (cancel + restart), or NoSpeculation (no spec was running).
    enum class Reconciliation { Match, Mismatch, NoSpeculation };
    Reconciliation reconcile(const std::string& speculation_text,
                             const std::string& final_text);
};
```

`PartialState` includes the latest partial's text, stable prefix length, time since last change, and the most recent VAD probability.

The match check uses a normalized token overlap, not strict-equality. `match_ratio = 0.9` means 90% of speculation tokens must appear in the final.

Rate-limit speculative restarts to prevent thrash; if `max_restarts_per_minute` exceeded, log a warning and disable speculation for the rest of the minute.

## Step 6 — Multilingual flow

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

Under Option C (deferred): no partials line, only the final transcript.

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

1. With the chosen STT backend running in Compose, speaking a sentence emits 3+ `PartialTranscript` events and one `FinalTranscript`. (Skip if Option C: only `FinalTranscript`.)
2. Speculation savings: median first-token-ready latency 300 ms lower than non-speculation baseline on a fixture set. (N/A under Option C.)
3. Multilingual: speaking in Russian routes to the Russian Piper voice (M3) and the LLM is prompted in Russian.
4. Mid-utterance revision: speak "set an alarm for ten — wait, eleven AM"; the spoken answer mentions 11 AM, not 10. (Under Option C: works trivially since we wait for the final.)
5. Memory rows have populated `lang` columns matching the detected language.
6. (Options A/B only) `voice_speculation_kept_total` and `voice_speculation_restarted_total` counters emit non-zero values; ratio is > 50 % in stable sentences.

## Risks specific to M5

| Risk | Mitigation |
|---|---|
| Streaming-engine choice deferred | Make decision early in M5; fallbacks to Option C if either A or B blocks |
| Custom wrapper complexity (Option A) | Vendor whisper.cpp snapshot; iterate against `examples/stream` |
| Speaches upstream drift (Option B) | Pin image digest; track upstream commit; one-line update PRs |
| Speculative restart thrash | Conservative defaults; rate-limit; emit warning on disable |
| Wrong language detection causes voice/prompt mismatch | Confidence threshold; fallback language; metric `voice_lang_low_confidence_total` |
| Multilingual STT slow on CPU | Configurable model size; benchmark at startup |
| Race: FinalTranscript arrives before speculation is set up | Reconciliation handles both orders; FSM tests cover the race |
| Speculative LLM run wastes GPU cycles | Hard cap by `speculation.max_restarts_per_minute`; report waste in metrics |

## Time breakdown

Per option (steps 2-7 are mostly identical; Step 1 differs):

| Step | A: Custom | B: Speaches | C: Defer |
|---|---:|---:|---:|
| 1 Streaming engine setup | 5 d | 0.5 d (compose only) | 0 |
| 2 STT client (Whisper / Speaches / non-streaming) | 2 d | 1.5 d | 1 d |
| 3 VAD → STT wiring | 1.5 d | 1.5 d | 1 d |
| 4 FSM SpeculativeThinking | 2 d | 2 d | 0 (skip) |
| 5 Speculation policy | 1.5 d | 1.5 d | 0 (skip) |
| 6 Multilingual flow | 1 d | 1 d | 1 d |
| 7 Tests + fixtures | 2 d | 2 d | 1.5 d |
| **Total** | **~15 d (~3 wk)** | **~10 d (~2 wk)** | **~5 d (~1 wk)** |
