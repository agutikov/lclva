# M7 — Barge-In

**Status: code-complete 2026-05-04** — Steps 1, 3, 4, 6, 7 landed; Step 2
verified by unit tests (`tests/test_barge_in.cpp`); Step 5 (manual
50-trial recorded validation) deferred to a separate dogfood pass when
real mic + speakers are wired up. Bug TODOs 1, 2, 4 closed; Bug 3 left
open with a note (existing inline comment in
`src/stt/realtime_stt_client.cpp` documents that the
`prefix_padding_ms` field is required by Speaches' Pydantic validator
even though Speaches itself rejects it as unsupported — dropping it
breaks the wider `session.update` flow, so the warn line stays).

**Estimate:** 1 week.

**Depends on:** M5 (partial transcripts), M6 (AEC wiring) **and M6B (AEC hardware acceptance — gates 1, 3, 4)**. All three are hard prerequisites — barge-in without *working* AEC is a non-starter in speaker mode (the assistant interrupts itself). M6 alone is necessary but not sufficient: the in-process APM wiring landed in M6, but on hardware where the codec DSP or speaker non-linearity defeats it (e.g., the dev laptop's ALC257), M6B's PipeWire system-AEC fallback is what actually keeps phantom triggers below the noise floor.

**Blocks:** nothing — M8 hardens what's already there.

## Goal

Speaker-mode barge-in works. The user can interrupt the assistant by speaking; within ~400 ms the assistant stops, drains its playback, and returns to listening. Tested against the success criteria from `project_design.md` §19: ≥ 90% correct cancellation in speaker mode.

The mechanics already exist (M0 cancellation cascade, M3 playback queue with sequence-no rejection, M5 partial transcripts). M7 wires the *detection* layer — turning AEC-cleaned VAD onsets during `Speaking` into `UserInterrupted` events — and validates end-to-end.

## Out of scope

- Wake-word trigger (Phase 3 / post-MVP).
- Adaptive thresholding to user-voice profile.

## Step 1 — Barge-in detector ✅

**Files:**
- `src/dialogue/barge_in.hpp`
- `src/dialogue/barge_in.cpp`

**Landed 2026-05-04.** The detector subscribes to `SpeechStarted` only
(rather than `subscribe_all`), which keeps its event queue narrow. It
reads FSM state via `fsm.snapshot()` rather than via a transition event
— `FsmSnapshot` was extended with an `entered_speaking_at` time_point
that the FSM stamps on every transition INTO `Speaking`, so the
cool-down window is anchored on real wall-clock and not on bus
dispatch ordering. The Apm pointer is optional (nullptr-safe);
`require_aec_converged + null Apm` short-circuits to "suppressed (aec)"
so headphone setups can disable the gate by flipping the config bool.
The detector exposes `set_on_fired(turn, ts)` which main.cpp wires to
`PlaybackEngine::note_barge_in` for the M7 §4 latency metric. Wiring
must happen BEFORE `start()` to avoid a brief window where SpeechStarted
fires before the callback is connected — `build_dialogue_stack`
constructs the detector but main.cpp owns the start.

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

## Step 2 — Cancellation cascade verification ✅

The cascade itself was implemented in M0 (`Fsm::handle_user_interrupted`). M7 verifies end-to-end timing and correctness with all real components:

1. `UserInterrupted` published.
2. `Fsm::handle_user_interrupted` bumps the active turn id, cancels the LLM token.
3. `LlmClient` observes the cancellation, aborts libcurl.
4. `TtsBridge` discards in-flight TTS results (turn-id mismatch).
5. `PlaybackQueue::invalidate_before` drains queued chunks.
6. Audio output silences within audio buffer length (~10 ms).
7. FSM transitions `Speaking → Interrupted → Listening`.

Each step has a metric / log line for tracing.

## Step 3 — Memory persistence policy ✅

**Landed 2026-05-04.** TurnWriter now subscribes to `PlaybackFinished`
in addition to the LLM lifecycle events, tracking a `played[]` bitmap
parallel to `sentences[]` (indexed by seq, not insertion order — the
M9 speculative path will eventually emit out-of-order sequences).
`handle_finished` builds the persisted text from `(cancelled ?
sentences[played] : sentences[all])`. A cancelled turn with zero
played sentences writes no row at all (Discarded outcome). The
`interrupted_at_sentence` column is set to the count of persisted
sentences, matching the existing schema. Two unit tests pin the
behaviour:

- `cancellation with played sentences writes Interrupted` — emits 2
  sentences, only seq=0 plays, cancellation persists the played
  text only.
- `cancellation with sentences emitted but none played writes nothing`
  — pre-M7 this case wrote the full emitted text as Interrupted; now
  it discards.

Already designed in §6 of project_design.md. Wire into the FSM's outcome observer (M1):

- Outcome `Discarded`: do not write the assistant turn to memory.
- Outcome `Interrupted`: write the assistant turn with `status='interrupted'` and `text` = concatenation of sentences whose `PlaybackFinished` was observed before `UserInterrupted` (i.e., what the user actually heard).

The "what the user actually heard" detail is important: a sentence that was synthesized but not yet started playback should not be in the persisted text. Enforce in the `TurnWriter` (M1, extended here):
- Track `last_played_seq` per turn (updated on `PlaybackFinished`).
- On interrupted outcome: persist `concat(sentences[1..last_played_seq])`.

## Step 4 — Latency-to-cancellation metric ✅

**Landed 2026-05-04.** `voice_barge_in_latency_ms` is a histogram
buckets `[10, 25, 50, 100, 150, 200, 300, 400, 600, 1000]`. The
detector's `on_fired` callback stores the publish ts atomically into
`PlaybackEngine::barge_in_publish_ns_`. The audio thread closes the
timer on the FIRST silent buffer it emits while the active turn no
longer matches the cancelled one (covers both the underrun path and
the pre-fill silence path). `consume_pending_barge_in_latency_ms()`
single-shot-pops the value; the TtsStack metrics poller observes it
into the histogram every 500 ms.

Also added: gauges `voice_barge_in_fires_total`,
`voice_barge_in_suppressed_total{cause=cooldown|aec|any}` mirroring
the detector counters from a 1 Hz main.cpp poller.

`voice_barge_in_latency_ms`: histogram of (UserInterrupted publish time → audio silence time). Audio silence = first audio buffer emitted after the cancel that contains zeros for the queued-chunk position.

Capture via:
- `BargeInDetector` records the publish time as `barge_in_started_at` on the FSM.
- Playback engine records `barge_in_silenced_at` when the first zero-out buffer is emitted post-cancel.
- The delta is logged + sampled into the histogram.

## Step 5 — Validation suite (deferred)

**Recorded fixtures in `tests/fixtures/barge-in/`:**
- `clean-speakers.wav` — TTS audio playing; user says "stop" cleanly mid-sentence. Expected: cancellation within 400 ms; outcome `interrupted`.
- `noise-speakers.wav` — TTS playing under moderate background noise; user says "stop". Expected: same.
- `headphones.wav` — same script with headphones (no echo path). Expected: same.
- `false-positive-tv.wav` — TTS playing; background TV news (someone else speaking). Expected: NO cancellation; `voice_barge_in_false_fires_total` does not increment.
- `false-positive-self.wav` — TTS playing; AEC-cleaned. Expected: NO cancellation.

**Test driver:** `tests/test_barge_in_validation.cpp` (gated by `ACVA_REAL_AUDIO=1`). Replays the WAV through the playback engine and the mic, observes barge-in detector behavior, asserts on cancellation latency and false-fire counts.

## Step 6 — UX polish ✅

**Landed 2026-05-04.** `Manager::on_event` now drops FinalTranscripts
whose normalised visible-character count (UTF-8 code points after
trimming whitespace + control chars) is below
`cfg.barge_in.min_real_utterance_chars` (default 3). Discarded
transcripts publish `UserInterrupted` so the FSM walks
`Transcribing → Interrupted → Listening` without invoking the LLM. The
test_manager test for "go" had to be widened to "go now" — under the
new policy the original was filtered as too short, which is the
intended production behaviour.

- After barge-in, the assistant should not greet "Yes?" or anything similar — just go quiet and listen. The user's next utterance is the next turn.
- Cancellation cascade fires `CancelGeneration` event (already in design); supervisor / metrics observe but no UI.
- If the user's interruption is brief (e.g., a cough that crosses the VAD threshold), the next user turn will likely produce a low-confidence final transcript. The Dialogue Manager should detect: if `FinalTranscript.text.length < min_real_utterance_chars` (default 3 chars after normalization), treat as a discarded utterance and do not run the LLM. Just transition back to `Listening`.

## Step 7 — Config ✅

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

## Demo command (planned)

### `acva demo bargein` — auto-injected interrupt during a chat turn

Same setup as `acva demo chat` (M1+M3), but extended: submits a prompt
known to produce ≥ 3 sentences ("Tell me a four-sentence story about
the moon"), waits for the first sentence to start playing, then
publishes `UserInterrupted` programmatically. Verifies the full
cancellation cascade fires.

Expected output:

```
demo[bargein] submitting long prompt; will inject interrupt after first sentence plays
  Once upon a time on the moon, a quiet astronaut waited.
demo[bargein] injecting UserInterrupted (turn=N, played=1 sentence)
demo[bargein] done: time_to_cancel_ms=87 sentences_played=1 sentences_dropped=2
              outcome=interrupted (M7 §6 persistence policy)
```

Failure modes:
- `time_to_cancel_ms > 400` → cancellation cascade is slow; check for blocking I/O on the dialogue subscriber thread.
- `outcome=completed` despite the interrupt → FSM didn't reach Speaking before we injected; increase the pre-inject delay or look for a stuck state machine.
- `sentences_played=0 + outcome=discarded` → cap-or-cancel race; nothing to play yet. Try a longer prompt.

What it doesn't cover: real-mic-driven barge-in (that's the manual
50-trial test below). It's purely the cancellation-cascade smoke —
proves the bus + Manager + TtsBridge + queue all drop in time and the
TurnWriter persists the right outcome. The mic + AEC + VAD path is
exercised by `acva demo capture` and `acva demo aec` separately.

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
| 8 Cleanup TODOs (below) | 0.5 day |
| **Total** | **~7 days = ~1 week** |

## TODOs / known issues to clean up before closing M7

**Status as of 2026-05-04 (M7 close):**

- **Bug 1 — pre_padding_ms missing in M5 streaming sink.** ✅ Fixed.
  Added `UtteranceBuffer::pre_buffer_snapshot()` and pushed the
  snapshot into `live_sink_` on `SpeechStarted` BEFORE
  `on_speech_started` adopts (and clears) the pre-buffer. Realtime STT
  now sees the same leading-phoneme prefix the M4B path already had.

- **Bug 2 — max_assistant_sentences hard-cancel.** ✅ Fixed.
  PromptBuilder appends "Reply in at most {N} short sentences." to the
  system content (skipped for the 100-sentence "effectively unlimited"
  default). Manager runs a tri-state cap machine: first sentence at
  the cap enters Pending, the next sentence emission triggers cancel
  + breaks the loop, with `cap_backstop = cap+1` as a hard ceiling
  the LLM cannot exceed even if it ignores the prompt. Net: the user
  hears one extra full sentence beyond the cap rather than a
  half-thought. Test `manager: max_assistant_sentences caps emission`
  was relaxed: emission ends at cap+1, and `cancelled` is now
  best-effort (the cancel may race natural stream completion).

- **Bug 3 — realtime STT prefix_padding_ms warn.** ⚠️ Left in place.
  The existing inline comment in
  `src/stt/realtime_stt_client.cpp:89-129` documents the conflict:
  Speaches' `PartialSession` Pydantic schema *requires* the field
  (omitting it fails union-validation against `TurnDetection |
  NotGiven` and silently drops the entire `turn_detection` block,
  including `create_response: false`), while Speaches' session.update
  handler then dispatches an `invalid_request_error` event for the
  field as "unsupported". The session itself applies cleanly with
  `create_response: false` in effect. Dropping the field would break
  the wider bring-up flow; the warn line is the one observable
  side-effect and is harmless. Revisit if Speaches' schema changes.

- **Bug 4 — Whisper hallucination on near-silence.** ✅ Fixed.
  `cfg.stt.min_utterance_rms` (default 200, ≈ -45 dBFS) gates the
  `UtteranceReady` publish in `AudioPipeline`. Slices below the
  threshold are dropped + logged + counted in
  `low_rms_drops_total()`. The M4 endpointer's `min_speech_ms`
  catches most cases; this is the belt-and-braces backstop for
  pre/post-padding-diluted utterances.

Two carry-over bugs surfaced during M5 dogfooding. Both touch the
audio + dialogue path, so M7 (which significantly reworks both for
barge-in) is the natural place to fix them.

- **Bug 1: M5 streaming sink misses `pre_padding_ms` window.** In
  `audio/pipeline.cpp`, the live-audio sink fires only between
  `SpeechStarted` and `SpeechEnded`. But the M4 endpointer waits
  `min_speech_ms` (200 ms) of above-threshold audio before firing
  `SpeechStarted`, and the rolling 300 ms `pre_padding_ms` is
  buffered only inside `UtteranceBuffer` — the M4B request/response
  path consumes it; the M5 streaming path doesn't. Net: the
  realtime STT loses ~200–500 ms of leading audio per utterance,
  occasionally dropping the first phoneme of the user's first word.
  Fix: at the `SpeechStarted` outcome, replay the buffered pre-pad
  samples into `live_sink_` before falling through. `UtteranceBuffer`
  already retains them; expose `recent_samples(duration)` and push
  them to the sink as one chunk. ~1 hour.

- **Bug 2: `max_assistant_sentences` cap silently truncates LLM mid-stream.**
  `Manager::run_one` cancels the LLM stream once the cap fires
  (default 6). The LLM has no idea — its output ends abruptly, the
  user hears a half-thought. Two-part fix: (a) thread the cap into
  the system prompt via `PromptBuilder::assemble_system_content`
  (one extra line: *"Reply in at most {max_assistant_sentences}
  short sentences."*); (b) let the in-flight sentence finish before
  cancel fires (defer cap-driven cancel to the next `LlmSentence`
  boundary, not the next `LlmToken`). Cancel-at-cap stays as a hard
  backstop. ~1–2 hours.

- **Bug 3: realtime STT sends an unsupported `session.update` field.**
  Surfaced 2026-05-03 in the production log
  (`/var/log/acva/acva-20260503-231726.log:33`):

  ```
  warn stt-realtime: server error: Specifying
    `session.turn_detection.prefix_padding_ms` is not supported.
    The server either does not support this field or it is not
    configurable.
  ```

  Speaches' OpenAI-realtime-compat surface accepts a subset of the
  upstream OpenAI schema; `prefix_padding_ms` under `turn_detection`
  isn't on it. The session keeps going (Speaches ignores the field
  and continues), but every startup logs a warn line. Drop the field
  from the session.update payload our realtime envelope sends; if
  we still need pre-pad framing, do it client-side via
  `UtteranceBuffer`'s rolling window (we already have it for the
  blocking-STT path). Look in `src/stt/realtime_envelope.cpp` for
  the session.update assembly. ~30 minutes.

- **Bug 4: phantom Russian transcripts on near-silence (Whisper
  hallucination).** Same log, line 16:

  ```
  trace final_transcript lang=ru text="Продолжение следует..."
  ```

  Whisper's training data includes a lot of Russian YouTube
  subtitles; on near-silent audio it loves to project "Продолжение
  следует…" ("To be continued...") or "Субтитры сделал DimaTorzok"
  (a prolific Russian subtitle creator). Without AEC the trigger was
  speaker-bleed silence; with PipeWire AEC active in M6B the trigger
  becomes ambient room noise during AEC convergence. The dialogue
  manager treats these as real user turns and prompts the LLM,
  wasting tokens and confusing the conversation.

  Fix: gate STT submission on a minimum-utterance-RMS threshold (or
  a min unique-pause-pattern check) before posting to Speaches. The
  M4 endpointer's `min_speech_ms` already filters most of these but
  not all — Whisper can hallucinate from the pre/post-padding even
  when the speech-detected window itself was empty. Cleanest place
  to add the gate is `audio::AudioPipeline` between
  `UtteranceBuffer::commit` and the live-sink dispatch.
  ~2–3 hours including a regression test that feeds the demo a 1 s
  silent buffer + asserts no `FinalTranscript` fires.
