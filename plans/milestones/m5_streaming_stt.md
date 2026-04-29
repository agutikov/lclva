# M5 — Streaming STT + Speculation

**Estimate:** 2–3 weeks. (Largest single milestone. Custom Whisper streaming wrapper + speculation FSM logic + multilingual flow.)

**Depends on:** M1 (Dialogue Manager wiring; LLM client cancellation), M4 (utterance pipeline; AudioSlice).

**Blocks:** M7 (full barge-in needs partials). M5 itself is the milestone where the FSM gains its `SpeculativeThinking` concurrent state.

## Goal

Three things land together:

1. **Streaming partial transcription**: whisper.cpp produces `PartialTranscript` events as the user speaks; a `FinalTranscript` closes the utterance.
2. **Multilingual**: Whisper detects the language per utterance; the language flows through prompt assembly, TTS voice selection, and memory `lang` columns.
3. **Speculative LLM start**: the Dialogue Manager opportunistically begins LLM generation against a stable prefix before the final transcript arrives, then either keeps or restarts based on reconciliation.

## Out of scope

- Whisper.cpp source modifications. We use the upstream library and wrap it.
- Production-quality multilingual evaluation across all Whisper-supported languages. M5 verifies a small set (en, ru, de, es, fr) on Common Voice fixtures.

## New deps

| Lib | Source | Purpose |
|---|---|---|
| whisper.cpp | upstream, built locally | STT engine (linked into the wrapper, not the orchestrator) |
| (orchestrator side) | nothing new | reuses libcurl, cpp-httplib |

## Step 1 — Whisper streaming HTTP wrapper

The wrapper is its own subproject; it ships in `packaging/whisper-server/` and is built independently. Without modifying whisper.cpp itself, we need:

- A long-lived process holding the model in memory.
- An HTTP API that streams partials.
- Sliding-window inference (whisper.cpp's `stream` example does this).

**Recommended approach:** vendor whisper.cpp's `examples/stream/stream.cpp` as a reference, write a C++ HTTP wrapper around it using cpp-httplib.

**HTTP API (the orchestrator's contract):**

- `POST /utterance/start` body `{"utterance_id": "u-42", "expect_lang": null}` → `{ "ok": true }`
  - Begins a new utterance session. Server allocates a sliding-window decoder.
- `POST /utterance/audio` body raw PCM `audio/x-pcm; rate=16000; channels=1` with header `X-Utterance-Id: u-42` → `204 No Content`
  - Append audio. Decoder runs incrementally.
- `GET /utterance/stream?id=u-42` → SSE stream of:
  ```
  event: partial
  data: {"text":"hello","stable_prefix_len":5,"lang":"en","seq":1,"confidence":0.85}

  event: final
  data: {"text":"hello world","lang":"en","confidence":0.91,"audio_duration_ms":1200,"processing_ms":340}
  ```
- `POST /utterance/end` body `{"utterance_id": "u-42"}` → `{ "ok": true }`
  - Forces the decoder to finalize and emit the `final` event.
- `GET /health` → `{"ok": true, "model": "small", "loaded": true}`.

**Files in `packaging/whisper-server/`:**
- `CMakeLists.txt`
- `main.cpp` — cpp-httplib server.
- `whisper_session.hpp/cpp` — wraps a single utterance's incremental decoder state.
- `README.md` — build instructions and API contract documentation.

**Build:** independent CMake project. Linked artifact is `whisper-server` binary, ~30 MB statically. Distribution is a separate concern; ships alongside the orchestrator.

**Streaming algorithm:** straightforward sliding window with overlap. Per whisper.cpp's `stream`:
- Run inference every `step_ms` (default 500) on the last `keep_ms + audio_since_last_step` (rolling).
- Emit a partial after each step. The stable prefix is the substring that hasn't changed from the previous partial.
- On `/utterance/end`, run a final inference over the full buffer and emit `final`.

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
| Multilingual smoke (gated) | `LCLVA_REAL_WHISPER=1` against Common Voice clips in en/ru/de/es/fr |
| End-to-end | speak in English; observe partials → final → speculation kept; speak with mid-sentence revision; observe restart |

## Acceptance

1. With `lclva-whisper.service` running, speaking a sentence emits 3+ `PartialTranscript` events and one `FinalTranscript`.
2. Speculation savings are measurable: median first-token-ready latency 300 ms lower than M3-style behavior on a fixture set, while the assistant says the right thing on revisions.
3. Multilingual: speaking in Russian routes to the Russian Piper voice (M3) and the LLM is prompted in Russian (system prompt selected by lang).
4. Mid-utterance revision: speak "set an alarm for ten — wait, eleven AM"; the FSM either reconciles or restarts; the spoken answer is correct (mentions 11 AM, not 10).
5. Memory rows have populated `lang` columns matching the detected language.
6. `voice_speculation_kept_total` and `voice_speculation_restarted_total` counters emit non-zero values; ratio is >50% in stable sentences.

## Risks specific to M5

| Risk | Mitigation |
|---|---|
| Whisper streaming wrapper complexity | Iterate against the upstream `stream.cpp` example; commit a vendored snapshot to keep build reproducible |
| Speculative restart thrash | Conservative defaults; rate-limit; emit warning on disable |
| Wrong language detection causes voice/prompt mismatch | Confidence threshold; fallback language; metric `voice_lang_low_confidence_total` |
| Multilingual Whisper slower on CPU | Configurable model size (B3 decision); benchmark medium vs small at startup |
| Race: FinalTranscript arrives before speculation is set up | Reconciliation handles both orders; FSM tests cover the race |
| Speculative LLM run wastes GPU cycles | Hard cap by `speculation.max_restarts_per_minute`; report waste in metrics |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Whisper wrapper (own subproject) | 5 days |
| 2 Whisper client | 2 days |
| 3 VAD → Whisper wiring | 1.5 days |
| 4 FSM SpeculativeThinking | 2 days |
| 5 Speculation policy | 1.5 days |
| 6 Multilingual flow | 1 day |
| 7 Tests + fixtures | 2 days |
| **Total** | **~15 days = ~3 weeks** |
