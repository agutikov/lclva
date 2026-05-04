# M9 — Streaming Partials + Speculative LLM Start

**Estimate:** 1.5–2 weeks once a partial-transcript source is available.

**Depends on:** M5 (streaming session, FinalTranscript flow), an STT
backend that emits `PartialTranscript` events. **Speaches as of
2026-05-02 does not** — see "Why M9 exists" below.

**Blocks:** none directly. M5–M8 deliver a working speech-to-speech
loop without speculation; M9 is a latency optimization (target 300 ms
median first-token-ready saved on stable-prefix utterances).

## Why M9 exists (separated from M5)

The original M5 plan included streaming partials + speculation as
mandatory acceptance gates. During M5 implementation we discovered
that **Speaches' realtime endpoint does not emit
`conversation.item.input_audio_transcription.delta` events**. Looking
at `realtime/input_audio_buffer.py`, the transcriber awaits the full
WAV via `transcription_client.create(...)` after `commit` and
publishes a single `transcription.completed` event — there's no
intermediate streaming. Three M5 acceptance gates depended on
partials and were therefore unreachable under the chosen Option B
backend without an upstream Speaches change.

Rather than block M5 closure on a multi-day Speaches PR with
uncertain merge timing — or retroactively switch to Option A (custom
whisper.cpp/server wrapper, a multi-week scope creep at this stage of
M5 implementation) — we lifted Steps 4–5 of M5 plus the speculation-
specific acceptance gates into this milestone. M5 ships without
speculation; M9 picks it up when partials become available.

## Goal

Two things land together:

1. **`PartialTranscript` events on the bus**, sourced from the STT
   backend's streaming output. Each carries `text`, `stable_prefix_len`,
   and a monotonic `seq` so consumers can detect stability windows.
2. **Speculative LLM start** in the Dialogue Manager: when a partial's
   stable prefix passes the speculation policy gates (length,
   stability, VAD-near-silent), opportunistically begin LLM generation
   against the partial. When `FinalTranscript` arrives, reconcile —
   keep the speculative run if the prefix matches, cancel and restart
   otherwise.

## Out of scope

- Multi-stream speculation (multiple LLM runs in flight simultaneously
  for different prefixes) — M9 keeps a single speculative slot.
- Speculation across language switches.

## Step 1 — Source partials from the STT backend

One of three options, decided at M9 start:

### Option A — PR Speaches to emit `transcription.delta`

faster-whisper supports streaming via `vad_filter=False, ...` and the
generator API. Speaches' `InputAudioBufferTranscriber._handler` would
need to:
- Iterate the segments generator
- For each new word, publish `ConversationItemInputAudioTranscriptionDeltaEvent`
- Publish `ConversationItemInputAudioTranscriptionCompletedEvent` at end

Probable PR shape: ~50 LoC in `realtime/input_audio_buffer.py`. Risk:
upstream review timing.

### Option B — Side-car streaming Whisper

Add a second backend (e.g., `whisper.cpp/examples/stream` wrapped by
the original Option-A custom server) to Compose. STT requests fork:
the realtime data channel still drives the buffered final, but a
parallel HTTP+SSE consumer feeds partials. More moving parts; uses
extra VRAM.

### Option C — Re-platform STT entirely

Drop Speaches for STT, keep it for TTS. Move STT to a custom
whisper.cpp wrapper that streams natively. Most flexibility, biggest
scope. Probably overkill given how much of M5 already runs cleanly
through Speaches.

**Recommendation at the time M9 starts:** start with Option A unless
Speaches has stagnated.

## Step 2 — `PartialTranscript` event publication

Once the backend emits deltas, `RealtimeSttClient`'s
`EventCallbacks::on_partial` (already wired through from M5) just
needs to publish onto the bus. Minimal client-side work — the M5
dispatcher already routes `conversation.item.input_audio_transcription.delta`
into a `PartialTranscript` event with `text` accumulated from deltas.

What's missing today and lands in M9:

- `stable_prefix_len` — distance into `text` that hasn't changed
  across the last N partials. Tracked by the dispatcher using a
  longest-common-prefix comparator against the previous emitted
  partial.
- `seq` — monotonic counter per turn, already wired.

## Step 3 — `SpeculativeThinking` FSM state

M5 deferred this. Now we add a concurrent sub-state.

**Files:** modify `src/dialogue/fsm.{hpp,cpp}`.

The FSM listens to `PartialTranscript`. When the speculation policy
fires (Step 4), the FSM enters `SpeculativeThinking` *while still
being in* `UserSpeaking` or `Transcribing`. The Dialogue Manager
submits an LLM request against the speculative prompt; the FSM
tracks the speculative turn id separately from the active turn id.

```cpp
struct FsmSnapshot {
    State state;
    TurnId active_turn;
    TurnOutcome outcome;
    bool speculative_in_flight;            // NEW
    TurnId speculative_turn;               // NEW
    std::uint32_t sentences_played;
    // ...
};
```

When `FinalTranscript` arrives:
- If the speculation matches: keep it; transition to `Speaking` as
  soon as the first sentence is ready (i.e., as before).
- If it diverges: cancel the speculative LLM (turn-id bump just for
  the speculative subtask), submit a fresh LLM request against the
  final transcript.

Both paths are normal cancellation. The `TurnContext` machinery from
M0 already supports this; we allocate two of them.

## Step 4 — Speculation policy

**File:** `src/dialogue/speculation.{hpp,cpp}` (new).

```cpp
struct SpeculationConfig {
    std::chrono::milliseconds hangover_ms{250};   // shorter than VAD's 600
    std::size_t min_chars = 20;
    std::chrono::milliseconds stability_ms{200};
    double match_ratio = 0.9;
    int max_restarts_per_minute = 8;
};

class SpeculationGate {
public:
    explicit SpeculationGate(SpeculationConfig cfg);

    // Called on each PartialTranscript and on VAD probability updates.
    // Returns true once when conditions are met to start a speculative
    // LLM run.
    bool should_speculate(const PartialState& state);

    enum class Reconciliation { Match, Mismatch, NoSpeculation };
    Reconciliation reconcile(const std::string& speculation_text,
                             const std::string& final_text);
};
```

`PartialState` includes the latest partial's text, stable prefix
length, time since last change, and the most recent VAD probability.

The match check uses normalized token overlap, not strict equality.
`match_ratio = 0.9` means 90% of speculation tokens must appear in
the final.

Rate-limit speculative restarts to prevent thrash; if
`max_restarts_per_minute` exceeded, log a warning and disable
speculation for the rest of the minute.

## Step 5 — Tests

| Test | Scope |
|---|---|
| `test_speculation.cpp` | Gate fires under stable conditions, rejects under instability; reconcile match/mismatch logic |
| `test_fsm.cpp` (extends) | New transitions: speculation start/keep/restart |
| End-to-end (manual) | Speak "set an alarm for ten — wait, eleven AM"; observe spoken answer mentions 11 AM, not 10 |

## Acceptance

1. With the chosen partial-transcript source running, speaking a
   sentence emits 3+ `PartialTranscript` events and one `FinalTranscript`.
2. **Speculation savings:** median first-token-ready latency 300 ms
   lower than the M5-baseline non-speculation run on a fixture set.
3. **Mid-utterance revision:** speak "set an alarm for ten — wait,
   eleven AM"; the spoken answer mentions 11 AM, not 10.
4. `voice_speculation_kept_total` and `voice_speculation_restarted_total`
   counters emit non-zero values; ratio is > 50% in stable sentences.

## Risks specific to M9

| Risk | Mitigation |
|---|---|
| Upstream Speaches PR stalls | Keep Option B (side-car) viable as a fallback path |
| Speculative restart thrash | Conservative defaults; rate-limit; emit warning on disable |
| Speculative LLM run wastes GPU cycles | Hard cap by `speculation.max_restarts_per_minute`; report waste in metrics |
| Race: `FinalTranscript` arrives before speculation is set up | Reconciliation handles both orders; FSM tests cover the race |

## Known issues to address in M9

- **Whisper hallucinations on the realtime STT path.** Surfaced 2026-05-04
  while running `scripts/barge-in-probe.py` against M6B Path B AEC.
  During barge-in conditions (user speaking under low-SNR audio),
  Speaches' faster-whisper backend falls back to memorised
  YouTube-subtitle artifacts:
  `Корректор А.Кулакова`, `Субтитры сделал DimaTorzok`,
  `Субтитры создавал DimaTorzok`, `Продолжение следует…`,
  `Спасибо за внимание`, English equivalents like
  `Thank you for watching` and `Subscribe and like`. The probe reported
  4/5 PASS but every one of the four was a hallucination.

  M7 Bug 4 added an RMS gate at `audio::AudioPipeline`'s `SpeechEnded`
  handler that drops `UtteranceReady` for low-RMS slices — but it ONLY
  covers the M4B request/response path. The M5 realtime path streams
  audio through `live_sink_` and commits on `SpeechEnded` directly; no
  RMS gate. Whisper sees the bytes, returns a hallucination, the
  realtime client publishes `FinalTranscript`, dialogue Manager
  treats it as a real user turn, the LLM runs against fake input.

  M9 owes the partials story; partials carry the same hallucination
  risk (an early "Субт" prefix would speculate against junk input).
  Both must be addressed when partials land:

  1. **Hallucination blocklist** in dialogue Manager — pattern-match
     on `FinalTranscript.text` and `PartialTranscript.text`; matches
     are suppressed before reaching the splitter / speculator. List
     lives in `cfg.stt.hallucination_patterns` (substring match,
     case-insensitive). Distinctive substrings only — bare
     "Пожалуйста" stays valid because real users say it.
  2. **Realtime-path RMS gate** — `RealtimeSttClient` keeps a rolling
     RMS of audio it pushed in the current utterance window; drops
     the resulting `FinalTranscript` (and any pending partials) if
     below `cfg.stt.min_utterance_rms`. Mirrors the M4B-side gate.
  3. **`scripts/barge-in-probe.py`** — same blocklist applied to its
     own PASS/FAIL classifier so the probe matches reality. Mirror of
     the C++ patterns; ~5 lines.

  These don't belong earlier than M9 because M9 is the first
  milestone to actually consume partials — and any half-fix that
  filters only finals leaves partials feeding speculation off
  hallucinated prefixes. Do it once, do it right.

  See `plans/milestones/m10_conversational_ux.md` for the
  complementary "address detection" angle: even a real, correctly-
  transcribed phrase like "уберите камеру" might not be addressed at
  the assistant. M9 handles the wire-level filter; M10 handles the
  semantic one.
