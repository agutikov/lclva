# M7 — Barge-In

**Estimate:** 1 week.

**Depends on:** M5 (partial transcripts), M6 (AEC). Both are hard prerequisites — barge-in without AEC is a non-starter in speaker mode (the assistant interrupts itself).

**Blocks:** nothing — M8 hardens what's already there.

## Goal

Speaker-mode barge-in works. The user can interrupt the assistant by speaking; within ~400 ms the assistant stops, drains its playback, and returns to listening. Tested against the success criteria from `project_design.md` §19: ≥ 90% correct cancellation in speaker mode.

The mechanics already exist (M0 cancellation cascade, M3 playback queue with sequence-no rejection, M5 partial transcripts). M7 wires the *detection* layer — turning AEC-cleaned VAD onsets during `Speaking` into `UserInterrupted` events — and validates end-to-end.

## Out of scope

- Wake-word trigger (Phase 3 / post-MVP).
- Adaptive thresholding to user-voice profile.

## Step 1 — Barge-in detector

**Files:**
- `src/dialogue/barge_in.hpp`
- `src/dialogue/barge_in.cpp`

A lightweight subscriber that:
1. Watches FSM state (snapshot or by subscribing to FSM transition events — add a `FsmTransition` event if not already present).
2. While state is `Speaking` and AEC is converged (delay estimate stable, ERLE > 15 dB):
   - On `SpeechStarted` event from the post-AEC VAD: publish `UserInterrupted{ turn = active_turn }`.

```cpp
class BargeInDetector {
public:
    BargeInDetector(event::EventBus& bus,
                    const dialogue::Fsm& fsm,
                    const audio::Apm& apm,
                    BargeInConfig cfg);
    void start();
    void stop();
};
```

**Config:**
```yaml
barge_in:
  enabled: true
  require_aec_converged: true
  min_aec_erle_db: 15.0
  min_vad_probability: 0.6     # tighter than normal endpointing; reduce false fires
  cool_down_after_turn_ms: 300 # don't fire immediately after Speaking begins
```

`cool_down_after_turn_ms` prevents firing on the user's own residual breath/lip sounds at the moment TTS starts.

## Step 2 — Cancellation cascade verification

The cascade itself was implemented in M0 (`Fsm::handle_user_interrupted`). M7 verifies end-to-end timing and correctness with all real components:

1. `UserInterrupted` published.
2. `Fsm::handle_user_interrupted` bumps the active turn id, cancels the LLM token.
3. `LlmClient` observes the cancellation, aborts libcurl.
4. `TtsBridge` discards in-flight TTS results (turn-id mismatch).
5. `PlaybackQueue::invalidate_before` drains queued chunks.
6. Audio output silences within audio buffer length (~10 ms).
7. FSM transitions `Speaking → Interrupted → Listening`.

Each step has a metric / log line for tracing.

## Step 3 — Memory persistence policy

Already designed in §6 of project_design.md. Wire into the FSM's outcome observer (M1):

- Outcome `Discarded`: do not write the assistant turn to memory.
- Outcome `Interrupted`: write the assistant turn with `status='interrupted'` and `text` = concatenation of sentences whose `PlaybackFinished` was observed before `UserInterrupted` (i.e., what the user actually heard).

The "what the user actually heard" detail is important: a sentence that was synthesized but not yet started playback should not be in the persisted text. Enforce in the `TurnWriter` (M1, extended here):
- Track `last_played_seq` per turn (updated on `PlaybackFinished`).
- On interrupted outcome: persist `concat(sentences[1..last_played_seq])`.

## Step 4 — Latency-to-cancellation metric

`voice_barge_in_latency_ms`: histogram of (UserInterrupted publish time → audio silence time). Audio silence = first audio buffer emitted after the cancel that contains zeros for the queued-chunk position.

Capture via:
- `BargeInDetector` records the publish time as `barge_in_started_at` on the FSM.
- Playback engine records `barge_in_silenced_at` when the first zero-out buffer is emitted post-cancel.
- The delta is logged + sampled into the histogram.

## Step 5 — Validation suite

**Recorded fixtures in `tests/fixtures/barge-in/`:**
- `clean-speakers.wav` — TTS audio playing; user says "stop" cleanly mid-sentence. Expected: cancellation within 400 ms; outcome `interrupted`.
- `noise-speakers.wav` — TTS playing under moderate background noise; user says "stop". Expected: same.
- `headphones.wav` — same script with headphones (no echo path). Expected: same.
- `false-positive-tv.wav` — TTS playing; background TV news (someone else speaking). Expected: NO cancellation; `voice_barge_in_false_fires_total` does not increment.
- `false-positive-self.wav` — TTS playing; AEC-cleaned. Expected: NO cancellation.

**Test driver:** `tests/test_barge_in_validation.cpp` (gated by `ACVA_REAL_AUDIO=1`). Replays the WAV through the playback engine and the mic, observes barge-in detector behavior, asserts on cancellation latency and false-fire counts.

## Step 6 — UX polish

- After barge-in, the assistant should not greet "Yes?" or anything similar — just go quiet and listen. The user's next utterance is the next turn.
- Cancellation cascade fires `CancelGeneration` event (already in design); supervisor / metrics observe but no UI.
- If the user's interruption is brief (e.g., a cough that crosses the VAD threshold), the next user turn will likely produce a low-confidence final transcript. The Dialogue Manager should detect: if `FinalTranscript.text.length < min_real_utterance_chars` (default 3 chars after normalization), treat as a discarded utterance and do not run the LLM. Just transition back to `Listening`.

## Step 7 — Config

```yaml
barge_in:
  enabled: true
  require_aec_converged: true
  min_aec_erle_db: 15.0
  min_vad_probability: 0.6
  cool_down_after_turn_ms: 300
  min_real_utterance_chars: 3
```

## Test plan

| Test | Scope |
|---|---|
| `test_barge_in.cpp` | unit-test the detector with a fake FSM + APM; thresholds enforced |
| `test_barge_in_validation.cpp` (gated) | the WAV-replay validation suite |
| Manual: 50-trial barge-in stress | speak interrupting prompts, log per-trial outcome and latency |

## Acceptance

1. Speaker mode (no headphones, AEC converged): 50-trial manual test, ≥ 90% correct cancellation within 400 ms; ≤ 1 false fire from the assistant's own voice.
2. Headphone mode: ≥ 95% within 300 ms.
3. `voice_barge_in_latency_ms` P50 ≤ 200 ms, P95 ≤ 400 ms.
4. `voice_turns_total{outcome="interrupted"}` and `{outcome="discarded"}` increment correctly per the persistence policy.
5. False-positive fixtures (`tv`, `self`) produce zero spurious cancellations.
6. Memory rows for interrupted turns contain only the played-out text.

## Risks specific to M7

| Risk | Mitigation |
|---|---|
| AEC convergence not yet stable when barge-in fires | `require_aec_converged + min_erle_db` gate |
| Cancellation latency exceeds 400 ms due to playback buffer | `audio.buffer_frames` is 10 ms; audio thread can drain to zero quickly |
| User-voice profile not learned | Out of scope; AEC + tuned VAD threshold are sufficient |
| Sentences mid-flight when barge-in fires get half-persisted | Persistence policy strict on `last_played_seq` |
| Brief cough / clearing throat triggers barge-in | `min_real_utterance_chars` filter on subsequent FinalTranscript |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Detector | 1 day |
| 2 Cascade verification | 0.5 day |
| 3 Persistence policy | 1 day |
| 4 Latency metric | 0.5 day |
| 5 Validation suite + fixtures | 2 days |
| 6 UX polish | 1 day |
| 7 Config + manual stress | 1 day |
| **Total** | **~6–7 days = ~1 week** |
