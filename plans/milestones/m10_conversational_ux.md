# M10 — Conversational UX

**Estimate:** ~1.5 weeks once M9 lands.

**Depends on:** **M9** (streaming partial transcripts) for the
adaptive endpointer; M8C wake-word is a related but independent
mechanism.

**Blocks:** none. M10 is post-MVP polish — the assistant works
without it; M10 makes it feel substantially more natural in
multi-occupant rooms and during long monologues.

## Why M10 exists

After M5 the always-on dialogue loop fires a turn on every detected
utterance. After M8C wake-word, the agent stays silent on
background speech but still treats every wake-word-gated utterance
as a complete prompt. That's still wrong in two ways:

1. **Long thinking pauses.** A user mid-sentence ("set an alarm for…
   uh… seven AM") triggers VAD `SpeechEnded` after `hangover_ms`
   (default 600 ms), and the partial transcript gets shipped as the
   final. The user has to repeat themselves.
2. **Indirect address.** Even with wake-word on, *every* utterance
   inside the follow-up window becomes a turn — including talk
   between the user and someone else in the room.

Both want richer signal than VAD or a wake phrase can provide. M10
delivers two complementary mechanisms.

## Out of scope

- The wake-word baseline itself — that's M8C Step 1 and continues to
  ship as the simple "is this for me?" gate. M10 layers semantic
  signals on top, not instead.
- Speaker diarization / "who is speaking" — separate problem, post-
  M10 if at all.
- Multi-turn context modelling (the user changing their mind across
  several turns) — that's a Manager / dialogue-policy concern, not
  a perception one.

## Step 1 — Adaptive endpointer

**Today (M5):** `Endpointer::on_frame` fires `SpeechEnded` after a
fixed `cfg.vad.hangover_ms` of below-offset audio (default 600 ms).
That's a good middle ground but wrong at both ends:

- Too aggressive when the user pauses mid-thought.
- Too lax when the user finishes crisply (extra 600 ms of latency
  for nothing).

**M10 introduces a partial-aware hangover.** The endpointer reads
the latest `PartialTranscript` (M9) at each `Speaking → Endpoint`
transition and picks a hangover from a small policy table:

| Heuristic on partial text | Hangover |
|---|---|
| Ends with `.`, `!`, `?` (terminal punctuation) | `cfg.vad.hangover_terminal_ms` (default 350 ms) |
| Ends with hesitation marker (`uh`, `um`, `hmm`, `er`) | `cfg.vad.hangover_hesitation_ms` (default 1500 ms) |
| Trailing comma / mid-clause / sentence < 5 words | `cfg.vad.hangover_midclause_ms` (default 900 ms) |
| Default (no signal) | `cfg.vad.hangover_ms` (default 600 ms — current behavior) |

All thresholds are configurable. A short language-specific hesitation
list (en, ru, de, ...) ships in `src/audio/hesitation_markers.cpp`.

**Files:**
- `src/audio/endpointer.cpp` — replace the constant-`hangover_ms`
  read with a callback that returns the current hangover. Pass the
  callback in via `EndpointerConfig`.
- `src/audio/hesitation_markers.{hpp,cpp}` — per-language token sets.
- `src/audio/pipeline.cpp` — subscribe to `PartialTranscript`,
  install a callback that classifies the latest partial and sets
  the next hangover.
- `tests/test_endpointer.cpp` — extend with each policy branch.

**Why this needs M9:** the adaptive policy reads partial transcript
text. M5's Speaches realtime endpoint emits only `FinalTranscript`,
which arrives *after* the endpointer has already decided to fire
`SpeechEnded`. M9 supplies streaming partials that the endpointer
can consult while still in `Speaking`.

## Step 2 — Address detection (semantic listen mode)

A separate gating layer above the wake-word path. Runs the latest
`FinalTranscript` (or, with M9, a stable partial prefix) through a
small classifier that decides "is this addressed to the assistant?"
and gates `Manager::enqueue_turn` accordingly.

**Two implementations** the user can pick between:

### Step 2.a — Heuristic classifier

Pure pattern matching:
- Imperative second-person verbs at the start ("set an alarm",
  "tell me", "what is", "open").
- Direct address tokens (the assistant's configured name(s),
  vocative pronouns).
- Question form (terminal `?`, interrogative starting tokens).

Cheap (sub-millisecond), zero new dependencies. Misses subtle cases
("I'd like to know…").

### Step 2.b — LLM classifier

A small purpose-built call to llama with a fixed system prompt:

```
You are a binary classifier. Reply with exactly "YES" or "NO".
Is the following user utterance addressed to the assistant
(seeking a response or action), or is it side-conversation /
self-talk / addressed to another human? "<transcript>"
```

Token cost ~5–10 tokens of output, ~50 tokens of input. Latency
~30–50 ms with Qwen2.5-7B Q4_K_M warm. Run in parallel with the
LLM-response path: if it returns NO before the response is more
than `cfg.dialogue.address_classifier_grace_ms` along, cancel the
response and `discard` the turn. If it returns YES (or the grace
window expires), let the response play.

**Files:**
- `src/dialogue/address_classifier.{hpp,cpp}` — both implementations
  behind a `Classifier` interface.
- Config:
  ```yaml
  dialogue:
    address_classifier:
      mode: off            # off | heuristic | llm  (default off)
      grace_ms: 200        # cancel-response window
  ```

Mode `off` (default) preserves M5–M8C behavior. Heuristic is
recommended for low-traffic personal assistants; LLM for shared
rooms where false-positives cost the user (and llama VRAM is paid
for anyway).

## Tests

- Endpointer policy unit tests: each heuristic branch fires with the
  matching partial, the chosen hangover takes effect, the
  `SpeechEnded` outcome is delayed/accelerated as expected.
- Address-classifier unit tests:
  - Heuristic: golden corpus of ~30 utterances split into addressed
    / not-addressed; assert ≥ 90% accuracy.
  - LLM: same corpus, gated on `ACVA_REAL_LLM=1`.
- Integration: live `acva` with `address_classifier.mode: heuristic`,
  speak side-conversation utterances, verify they don't trigger
  TTS playback (turn marked `Discarded`).

## Acceptance

1. **Adaptive endpointer holds long pauses.** Speaking "set an alarm
   for… uh… seven AM" with the hesitation lookup populated does NOT
   fire `SpeechEnded` until after the full utterance completes.
2. **Crisp endings are faster.** Speaking a complete sentence that
   ends with `?` causes `SpeechEnded` ~250 ms sooner than the M5
   default — measurable in the soak's per-turn-stage latency.
3. **Address classifier off (default) is unchanged from M8C.**
4. **Heuristic classifier rejects side-conversation.** With it on,
   speaking "I'll grab coffee" mid-room does not produce a turn.
   `voice_turns_total{outcome="discarded"}` increments instead.
5. **LLM classifier latency tolerable.** Median classifier round-
   trip ≤ 60 ms with Qwen2.5-7B-Q4_K_M warm. Full-stack effect on
   first-audio P50 ≤ 30 ms (the classifier runs in parallel with
   prompt assembly).

## Risks specific to M10

| Risk | Mitigation |
|---|---|
| Adaptive endpointer waits forever on hesitation, user gives up | Per-mode timeout caps in `EndpointerConfig`; absolute max `hangover_ceiling_ms` (default 3000 ms) regardless of policy |
| Heuristic markers wrong for the user's language / dialect | All markers configurable per-language; fall back to the M5 default hangover when no marker list is loaded |
| LLM classifier mis-fires on imperative-but-not-addressed ("close that door" to a pet) | Heuristic is the safer default; LLM mode is opt-in for users who want stronger filtering and accept the latency |
| Two classifiers (wake-word + address) drift on what counts as "addressed" | Document precedence: wake-word gate first (M8C); address classifier inside its follow-up window (M10) |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Adaptive endpointer | 3 days |
| 2.a Heuristic classifier | 1.5 days |
| 2.b LLM classifier + parallel cancel | 2.5 days |
| Tests + acceptance | 1.5 days |
| **Total** | **~8.5 days = ~1.5 weeks** |
