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

## Hallucination-handling layer (companion to M9)

M9 owes the wire-level hallucination filter — pattern blocklist on
`FinalTranscript.text` + realtime-path RMS gate (see
`plans/milestones/m9_speculation.md` "Known issues to address in M9"
for the full bug history from the 2026-05-04 barge-in-probe run).
M10's address-detection layer is the semantic complement: even a
real, correctly-transcribed phrase isn't necessarily addressed at
the assistant.

Three modes the address classifier should treat as "not-addressed",
all of which arrive looking like normal `FinalTranscript` events:

1. **Sub-utterance content the M9 blocklist missed.** New Whisper
   hallucinations (`Спасибо за просмотр`, `Pewdiepie`, etc.) drift
   into the wild as Whisper updates; M9 list is finite. Heuristic
   classifier (Step 2.a) catches these via "no imperative verb /
   no question form / no address token" → not addressed.
2. **Side conversation.** User says something to a person in the
   room while the assistant is speaking; mic catches it; STT
   transcribes correctly. Should not interrupt the assistant.
3. **Self-talk / thinking aloud.** "Hmm, where did I…" — real
   speech, addressed at nobody. Same heuristic filter applies.

This is why the barge-in-probe failure on 2026-05-04 (4/5 false
PASS, all hallucinations) belongs in M9+M10 jointly, not as an M7
patch: M9 catches the obvious YouTube-subtitle artifacts at the
wire layer; M10 catches the broader "speech-but-not-for-me" class
above the wire. A useful sanity check at M10 close: re-run
`scripts/barge-in-probe.py` and expect 5/5 with content matching
what the user actually said.

## TTS pacing — inter-sentence silence at high tempo

### Observation

At `cfg.tts.tempo_wpm: 240` (≈ `speed=1.5`) words feel quick but
the gaps between them feel disproportionately long, and overall
speech reads as fragmentary / chopped. Lowering `tempo_wpm` to 200
relieves the impression. The user's framing: *delays between words
not so small*, with the followup question of whether synthesizing
at native cadence and time-stretching playback would sound less
chopped.

### What we expose today

The full TTS surface in `cfg.tts` and the playback-side coupling:

| Knob | Where | Notes |
|---|---|---|
| `base_url` | `tts.base_url` | Speaches root, e.g. `http://127.0.0.1:8090/v1` |
| `voices[lang]` → `model_id`, `voice_id` | `tts.voices` (alias) → registry → `tts.voices_resolved` | Per-language route, alias from `models.tts` |
| `fallback_lang` | `tts.fallback_lang` | When detected lang has no entry |
| `tempo_wpm` | `tts.tempo_wpm` | Translated to `speed = tempo_wpm / 160` in `OpenAiTtsClient::build_body` |
| `request_timeout_seconds` | `tts.request_timeout_seconds` | libcurl timeout per sentence |
| `prefill_ms` | `playback.prefill_ms` | Per-turn pre-buffer before first frame plays (M4B fix; 100 ms default) |

Wire payload (Speaches' `POST /v1/audio/speech`):
`{ "model": ..., "input": ..., "voice": ..., "speed": ..., "response_format": "pcm" }`.
That's it — `model`, `voice`, `input`, `speed`, `response_format`.

What Piper actually supports underneath (from `piper.cpp`):

| Flag | Default | Units | Scales with `length_scale`? |
|---|---|---|---|
| `length_scale` | 1.0 | mult. on phoneme duration | (this is the knob) |
| `noise_scale` | 0.667 | generator noise | independent |
| `noise_w` | 0.8 | per-phoneme duration jitter | independent |
| `sentence_silence` | 0.2 | **seconds** appended after each sentence | **no — fixed seconds** |
| `phoneme_silence PHONEME N` | none | **seconds** appended after named phonemes | **no — fixed seconds** |
| `speaker` | 0 | speaker id (multi-speaker models) | independent |

**Speaches' `/v1/audio/speech` does not pass any of those through.**
`CreateSpeechRequestBody` in `speaches/routers/speech.py` accepts only
the OpenAI-standard fields plus `stream_format` / `sample_rate`. No
`extras`, no `extra_body`, no Piper-side endpoint. `speed` (which
becomes `length_scale = 1/speed` internally) is the only timing knob
that crosses the wire today.

### Root cause

Piper appends `sentence_silence` seconds of zero PCM after each
sentence and **the value is in seconds, not in phonemes** — so
`length_scale=0.667` (i.e. `speed=1.5`) shrinks the words by 33%
but the trailing 200 ms silence stays at 200 ms. The relative
contribution of silence to total time grows roughly as `speed`,
which is exactly the "chopped" perception. Per-phoneme silence
entries baked into a specific voice's `*.onnx.json` (if any) have
the same property.

Our `SentenceSplitter` issues one Piper call per sentence boundary,
so this trailing silence lands between every `LlmSentence` we play
back. At speed=1.0 (160 wpm) the 200 ms gap sits between ~750 ms
of speech and reads as natural punctuation; at speed=1.5 it sits
between ~500 ms of speech and reads as a stutter.

Intra-sentence pauses (the predicted gaps at commas / dashes) are
encoded as VITS phoneme durations and *do* scale with
`length_scale`, so they shouldn't be the culprit unless the user's
voice has aggressive `phoneme_silence` entries — verify by
inspecting the voice's `.onnx.json` in `${ACVA_MODELS_DIR}` if
Step 0 below doesn't fully resolve the perception.

### Three paths investigated

**Path A — client-side trailing-silence trim (no deps; recommended first).**
Post-process each sentence's PCM in `OpenAiTtsClient` (or
`TtsBridge` after the streaming sink completes a sentence): scan
the tail for the final non-silent sample (RMS below
`cfg.tts.silence_floor_dbfs`, default -50), then trim to a
configurable `cfg.tts.inter_sentence_silence_ms` (default e.g.
80 ms). Optionally make the target proportional to `tempo_wpm`
(`silence_ms ≈ 200 * 160 / tempo_wpm`) so the user gets natural
pauses at native cadence and tight pauses at speed-up. **No new
dependencies, ~1 day.** Addresses the dominant cause; leaves
intra-sentence punctuation pauses untouched (which is what we
want — those are paragraph rhythm).

**Path B — synthesize at native + pitch-preserving time-stretch
on playback (the user's suggestion).**
Send `speed=1.0`, time-stretch the PCM by `tempo_wpm / 160` before
the playback queue. Quality is best with a real PSOLA / phase-
vocoder library. Naïve resampling won't work — that shifts pitch
and sounds chipmunky. Candidate libraries (from grounding
research):

| Library | License | Streaming-friendly | Notes |
|---|---|---|---|
| Rubber Band | GPL-2.0 (commercial available) | yes (R3 engine) | Best perceptual quality; **GPL is a license problem** for acva unless we buy a commercial license |
| Signalsmith Stretch | MIT, header-only | yes, low-latency by design | Modern phase-vocoder; recommended pick if Path A is insufficient |
| SoundTouch | LGPL-2.1 | yes (WSOLA) | Safe LGPL middle ground; decent for speech, some artifacts |
| SoX `tempo` effect | GPL-2.0 | batch, not streaming-friendly | Skip |

**Tradeoffs.** Path B compresses words and silences uniformly, so
it solves the chopped feel structurally. It also opens the door to
a "play at 1.5x" listen-mode independent of the synth tempo. Cost:
~50–150 ms added to time-to-first-audio (time-stretchers need a
block of input before they emit), a new dependency, and an
allocation/CPU cost on every sentence. **~2–3 days** including
benchmarking on the 4060 box and confirming no underruns.

**Path C — patch Speaches to forward Piper-native params.**
Add an `extras` field to `CreateSpeechRequestBody` that we use to
send `sentence_silence` and `length_scale` directly. Cleanest
architecturally because Piper already has the right knob — we
just can't reach it. Cost: a fork (or upstreaming PR) plus the
ongoing burden of carrying a Speaches patch. Not worth it given
Path A covers most of the perceptual problem and Path B is the
right structural answer when we want it.

### Recommended sequencing inside M10

0. **Step 0 (TTS pacing).** Land Path A first — small, no deps,
   measurable. Acceptance: at `tempo_wpm: 240`, the 50-trial
   `acva demo chat` run subjectively reads as continuous speech;
   `voice_tts_audio_bytes_total / voice_tts_first_audio_ms_count`
   ratio drops by ≥ 25% (less zero-PCM tail per sentence).
1. Adaptive endpointer (existing Step 1).
2. Address detection (existing Step 2.a / 2.b).
3. **Step 3 (revisit if needed).** If Step 0 doesn't fully resolve
   the chopped feel — or once a "listen at 1.5x" mode is on the
   roadmap — pull in Signalsmith Stretch (MIT, header-only) and
   add Path B as `cfg.playback.time_stretch.enabled`. Default
   off; opt-in until measured against Step 0's result.

### Open question (TTS-A)

Verify whether the default `en-amy` and `ru-irina` voices ship
non-empty `phoneme_silence` arrays in their `.onnx.json`. If yes,
those need either Path C (forwarding) or a client-side spike
removal, since they're independent of `length_scale`. Defer until
Step 0 is in and we can A/B against actual recorded output.

## Time breakdown

| Step | Estimate |
|---|---|
| 0 TTS pacing — trailing-silence trim (Path A) | 1 day |
| 1 Adaptive endpointer | 3 days |
| 2.a Heuristic classifier | 1.5 days |
| 2.b LLM classifier + parallel cancel | 2.5 days |
| Tests + acceptance | 1.5 days |
| (Optional) 3 Time-stretch playback (Path B) | 2-3 days |
| **Total (without Path B)** | **~9.5 days** |
