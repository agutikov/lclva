# M6B — AEC hardware verification + system-AEC fallback

**Status:** planned. Inserted between M6 (in-process APM, code-complete)
and M7 (barge-in). M6B exists because M6's hardware acceptance gates
(1, 3, 4) are blocked on this dev workstation's audio path —
specifically the laptop ALC257 codec's built-in DSP and the speaker
amplifier's non-linearity at usable volume make in-process WebRTC APM
ineffective. M7 cannot proceed without working echo cancellation:
without it, the user can't speak over TTS without triggering phantom
barge-ins, and the half-duplex gate that protects production today
defeats the whole point of barge-in.

**Estimate:** 3–5 days.

**Depends on:** M6 (in-process APM wiring, LoopbackSink, demos,
metrics — all landed).

**Blocks:** M7 barge-in.

---

## 1. Goal

Reach a state where, on the dev workstation, **at least one** of the
following AEC paths produces ERLE > 25 dB and `voice_vad_false_starts_total
< 1/min` during continuous TTS playback (M6 acceptance gates 1 + 4):

- (A) In-process `webrtc::AudioProcessing` (the M6 path), with whatever
  signal-conditioning fixes are needed to get it working.
- (B) PipeWire's `module-echo-cancel` (which uses the same
  `webrtc-audio-processing` library system-side, but consumes the raw
  ALSA mic feed before any codec DSP can touch it via PipeWire).

Either path closes M6's acceptance gates. Path A is preferable for
portability (no PipeWire dependency). Path B is acceptable as the
production default on Linux desktops where PipeWire is the audio
server, with Path A still wired for non-PipeWire systems and as a
verification baseline.

## 2. Out of scope

- A custom AEC implementation (NLMS, Kalman, etc.). WebRTC APM is
  the upstream-maintained reference; rolling our own is a multi-month
  effort with no upside.
- Multi-microphone AEC. The dev hardware has a single DMIC pair seen
  as one stream; beamforming is post-MVP.
- AEC on Bluetooth headsets. Headphone-only mode bypasses the AEC
  problem entirely (no acoustic loop) and is documented as the safe
  fallback.
- AGC tuning. M6's `gain_controller1` is functional; the Step 7
  re-baseline of VAD thresholds covers the level-side calibration.

---

## Step 0 — Pin the failure mode (✅ done in M6 § 5.2)

Captured in `docs/aec_report.md` § 5.2. The hardware demo
(`acva demo aec-hw`) confirms:

- Loopback ring fills at correct rate (48 000 frames/s).
- APM processes 100 frames/s, delay estimator converges to 28 ms.
- Mic captures the chirp + pink noise stimulus at RMS 5000–6300.
- ERLE pinned at 0.2 dB. Cleaned mic RMS unchanged from raw.

So APM is fed correlated reference + mic at the right time base, finds
the delay, and still fails to cancel. Three contributing causes
ranked in `docs/aec_report.md` § 6: speaker non-linearity (likely),
codec DSP residual processing (likely), WebRTC API quirk
(low-likelihood). Step 1 captures ground-truth signals so analysis
isn't a guessing game; Step 2 runs the configuration probes
informed by Step 1's findings.

## Step 1 — Capture ground-truth signals + offline analysis

Rather than blindly tweaking config knobs and re-running
`acva demo aec-hw`, capture the actual audio at every stage of
the speaker→air→mic→APM chain into WAV files. Once we have
those three signals on disk, any standard tool (numpy/scipy,
ffmpeg, sox, Audacity, Python notebook) can answer all three
diagnostic questions definitively without rebuilding `acva`
once.

This is the highest-leverage step in M6B: every subsequent
decision (Steps 2–4) hinges on what these recordings show.

### 1.1 New demo: `acva demo aec-record`

**File:** `src/demos/aec_record.cpp` (new), `src/CMakeLists.txt`,
`src/demos/demo.cpp` (registry entry).

**CLI surface:**

```
acva demo aec-record [--text TEXT] [--out-dir DIR] [--lang LANG]

  --text TEXT      Sentence to synthesize. Default: a fixed mix of
                   words + numbers + a high-frequency token, in en
                   ("The quick brown fox jumps over the lazy dog.
                   One two three four five. Sssss.").
  --out-dir DIR    Where to drop the three WAV files.
                   Default: /tmp/acva-aec-rec/
  --lang LANG      tts.fallback_lang override. Default cfg value.
```

**Pipeline inside the demo:**

1. **Synthesize the original.** `OpenAiTtsClient::submit` against
   Speaches with the configured voice. Accumulate every
   on_audio chunk into a single buffer at the voice's native
   sample rate (typically 22050 Hz for Piper). Write
   `<out-dir>/original.wav` (mono int16).

2. **Spin up the audio loop.** Same wiring as `acva demo aec-hw`:
   `PlaybackEngine` + `LoopbackSink` + `CaptureEngine` + a
   standalone `audio::Apm` instance. **Do NOT** route through
   `AudioPipeline` — the demo bypasses it so the raw 48 kHz
   mic stream is accessible directly from the `CaptureRing`,
   without VAD / endpointer / utterance assembly side-effects.

3. **Resample original 22050 → 48000 Hz** via the existing
   `audio::Resampler` (soxr High). Enqueue chunks into the
   `PlaybackQueue` at realtime pace (matches what production
   does). Mark the last chunk `end_of_sentence` so the engine
   fires `PlaybackFinished` cleanly.

4. **Drain the capture ring on a worker thread.** For each 480
   sample block from the `CaptureRing`:
    - Append to `raw_48k_buffer` (later → `raw_recording.wav`
      at 48 kHz mono).
    - Resample 48 → 16 kHz via a persistent soxr instance.
    - Buffer into a 160-sample apm-frame chunker (same
      pattern as `AudioPipeline::apm_carry_`).
    - For each complete 10 ms chunk, call `apm.process(chunk,
      capture_time)` and append the cleaned output to
      `aec_16k_buffer` (later → `aec_recording.wav` at 16 kHz
      mono).

5. **Stop conditions.** Stop draining 1 second after
   `PlaybackFinished` so we capture the room tail. Hard cap
   at the original's duration + 5 s so a missing
   `PlaybackFinished` (silent device) doesn't hang.

6. **Write the WAV files** using the existing
   `make_wav` helper (in `src/stt/openai_stt_client.cpp`,
   move it to `src/audio/wav.{hpp,cpp}` first so the demo
   can include it without dragging in libcurl). Three files:

    | File | Sample rate | Channels | Source |
    |---|---:|---:|---|
    | `original.wav`       | 22050 Hz | 1 | exact bytes from TTS, before any speaker |
    | `raw_recording.wav`  | 48000 Hz | 1 | mic input, before APM |
    | `aec_recording.wav`  | 16000 Hz | 1 | mic input, after APM |

7. **Print summary.** RMS of each, durations, peak levels,
   `original` → `raw_recording` cross-correlation peak (with
   delay in ms — gives the physical air delay directly).
   Then point the user at the offline-analysis script.

### 1.2 Offline analysis script

**File:** `scripts/aec_analyze.py` (new). No build dependency
on the C++ project — pure Python, optional `numpy + scipy +
matplotlib` for charts. Plain stdout numbers without chart
deps.

**What it computes** from `original.wav`, `raw_recording.wav`,
`aec_recording.wav`:

1. **Delay between original and raw (cross-correlation peak).**
   Resample both to 16 kHz first. The lag at the peak is the
   physical speaker→air→mic delay. Should match what APM's
   internal `delay_ms` reports (~28 ms in the failing run).

2. **Per-band attenuation: original vs raw.** Compute STFTs of
   original and raw_recording. Plot the per-frequency-bin
   energy ratio. If the codec DSP suppresses 1 kHz tones, the
   plot will show a deep notch around 1 kHz; broadband content
   (pink noise) will show a flatter ratio. This is the
   definitive proof of where the codec eats the signal.

3. **Linearity check.** Compare a section of original at low
   amp vs the same section at high amp (we'd run the demo
   twice — once at amp=0.10, once at amp=0.50). If the high-amp
   raw is more than a clean amplitude scaling of the low-amp
   raw, the speaker chain is non-linear (compression / clipping).

4. **AEC effectiveness: raw vs aec, time-aligned.** Subtract
   `aec_recording` from a delay-aligned `raw_recording`. The
   residual should have ~0 energy at frequencies APM can
   cancel and ~mic-level energy at frequencies it can't. This
   is the operational ERLE measurement, independent of APM's
   self-reported number.

5. **Generate charts** (when matplotlib is available):
    - `<out-dir>/01_waveforms.png` — three signals overlaid,
      first 2 s.
    - `<out-dir>/02_spectrogram.png` — three signals, side by
      side.
    - `<out-dir>/03_attenuation.png` — original→raw ratio per
      frequency.
    - `<out-dir>/04_aec_residual.png` — `raw - aligned(aec)`
      energy per frequency.

**Stdout output** (always):

```
$ scripts/aec_analyze.py /tmp/acva-aec-rec
original.wav        : 4.21 s, RMS 7820, peak 16382
raw_recording.wav   : 5.20 s, RMS 1240, peak  6210
aec_recording.wav   : 5.20 s, RMS 1230, peak  6190

physical delay      : 32.1 ms (cross-correlation peak)
APM-side delay      : (read from /var/log/acva/...; expected match)

per-band attenuation (original → raw):
  100-300 Hz   :  -8 dB
  300-1000 Hz  : -22 dB     ← speaker rolloff + codec NS
  1000-3000 Hz : -18 dB
  3000-8000 Hz : -34 dB     ← codec aggressive HF cut

ERLE per band (raw → aec):
  100-300 Hz   :   1.2 dB
  300-1000 Hz  :   0.3 dB     ← APM not cancelling — see analysis
  1000-3000 Hz :   0.5 dB
  3000-8000 Hz :   0.0 dB

verdict: codec DSP suppresses the reference signal so much that
         APM has no acoustic signal to cancel. Try USB mic.
```

The verdict line is rule-based: a few simple thresholds on the
per-band numbers map to the three hypotheses in
`docs/aec_report.md` § 6.

### 1.3 Acceptance for Step 1

- `acva demo aec-record` produces three valid WAV files that
  open cleanly in Audacity/ffplay/sox.
- `scripts/aec_analyze.py` runs cleanly on the recorded WAVs
  and prints the per-band table.
- The combined output **identifies the dominant failure mode**
  (speaker non-linearity / codec DSP suppression / both /
  neither) without further guesswork. Steps 2–4 then act on
  that finding.

### 1.4 Why this is the right shape

- **Bypassed `AudioPipeline`** — the demo's APM run is
  isolated from the production pipeline, so a bug in
  `apm_carry_` chunking can't masquerade as a hardware issue.
- **Three signals at three sample rates** — captures every
  acoustic and digital transformation in the chain at full
  fidelity. Nothing's averaged or lost.
- **Offline analysis decouples C++ work from diagnostic
  iteration.** Once you have the WAVs you can re-analyze
  with different scripts, manually compare in Audacity, share
  with someone for a second opinion, etc., without
  re-recording.
- **Single source of truth for `aec_report.md`.** The Step 5
  doc update can paste actual numbers from the analysis
  script instead of speculating.

### 1.5 Time

| Sub-step | Cost |
|---|---:|
| `acva demo aec-record` C++ implementation | 1 d |
| Move `make_wav` to `src/audio/wav.{hpp,cpp}` | 0.5 h |
| `scripts/aec_analyze.py` (numpy version) | 0.5 d |
| Optional matplotlib charts | 0.5 d |
| Run + interpret on dev workstation | 0.5 d |
| **Step 1 total** | **~2.5 d** |

This is one extra day vs the original Step 1.1 + 1.2 ad-hoc
probes, in exchange for a permanent diagnostic tool the project
keeps using whenever AEC behavior changes (driver update,
hardware swap, M6B Step 3 acceptance retest, etc.).

## Step 2 — Path A diagnosis: rule in/out the in-process AEC

(Was Step 1 in the original M6B draft — kept as a follow-on so the
ad-hoc probes inform the Step 1 recording's interpretation.)

Two cheap experiments, each closes one hypothesis. Run after
Step 1 has produced the three WAVs so each probe can be
re-analyzed with the same offline script.

### 2.1 Lower-amplitude retest

The `demo aec-hw` stimulus runs at amp=0.5 (-6 dBFS) — comfortable
headroom on a USB mic but potentially clipping into the laptop's
class-D amp's DRC region. Drop to amp=0.10 (-20 dBFS) by adding a
`--amp` flag (or by editing the constant) and re-run. If ERLE
climbs into the >10 dB range, **speaker non-linearity at high level
is the dominant contributor** and the in-process path is salvageable
with a documented max-volume guidance.

Acceptance for this sub-step: a single demo run plus a one-paragraph
note in the report. ~30 minutes.

### 2.2 USB-microphone bypass

A USB conferencing mic (Blue Yeti, Jabra Speak series, or any USB
class-compliant device) presents the host with a clean digital mic
stream that bypasses the laptop codec's DSP entirely. Wire it via
`cfg.audio.input_device: "USB"` (substring match). Re-run
`acva demo aec-hw`. If ERLE > 25 dB after convergence on the chirp
section, **codec DSP is the dominant contributor** and the
in-process path works fine on hardware without that interference.

Acceptance for this sub-step: demo passes (exit code 0) on the USB
mic, plus the same single-paragraph note. ~1 hour, including
plug-and-play.

### 2.3 Decision

After 2.1 + 2.2 (informed by Step 1's recordings):

- If 2.1 alone gets ERLE > 25 dB → ship Path A with a max-volume
  note in `docs/troubleshooting.md`. Skip Step 3.
- If 2.2 gets ERLE > 25 dB but 2.1 doesn't → Path A works on
  external hardware but not on the integrated codec. Ship Path A as
  the default; document USB-mic recommendation for users hitting
  the codec-DSP wall.
- If neither — Path A is dead on this hardware class. Proceed to
  Step 3 (Path B, system AEC).

## Step 3 — Path B: PipeWire system-AEC fallback (was Step 2 in original draft)

PipeWire ships `module-echo-cancel`, which loads
`webrtc-audio-processing` system-side and presents a cleaned source.
Crucially, it consumes the raw ALSA mic stream before any codec DSP
gets to apply additional processing (the codec DSP runs on the
per-application stream after PipeWire mixes).

### 3.1 Manual smoke test (no code)

```sh
pactl load-module module-echo-cancel \
    source_name=acva-source \
    sink_name=acva-sink \
    aec_method=webrtc \
    aec_args="'analog_gain_control=0 digital_gain_control=1 noise_suppression=1 high_pass_filter=1'"
```

Then re-run `acva demo aec-hw` with `cfg.audio.input_device:
"acva-source"` (substring) and `cfg.audio.output_device: "acva-sink"`.
The system AEC should cancel the speaker bleed before our APM gets
involved; in the demo we'd observe `mic_rms` drop dramatically
during the stimulus. ERLE-as-reported-by-our-APM stays ~0 (nothing
left to cancel) but the M4 VAD won't false-fire.

Smoke result drives whether we add a permanent code path.

### 3.2 Permanent wiring

Add `cfg.apm.use_system_aec` (bool, default `false`):

- When `true`, on startup `acva` invokes the equivalent of the
  `pactl load-module` above (via the PulseAudio C client API or
  by shelling `pactl`), records the resulting source/sink IDs,
  and overrides `cfg.audio.input_device` / `output_device` with
  the new virtual ones.
- On shutdown, `acva` unloads the module so subsequent runs
  don't pile up duplicate AEC chains.
- `cfg.apm.aec_enabled` remains independent — typically set to
  `false` when `use_system_aec` is `true` to avoid double-AEC.

Files:
- `src/orchestrator/system_aec.{hpp,cpp}` — new RAII helper that
  load/unloads the module via `pactl`. Lives next to the other
  orchestrator stacks.
- `src/orchestrator/bootstrap.cpp` — call-site, before
  `install_alsa_sidestep` (PipeWire needs to see the ALSA config
  unmodified for source-name resolution).
- `src/config/config.hpp` — `use_system_aec` knob + validation.
- `config/default.yaml` — documented as the recommended default on
  PipeWire systems where Path A doesn't pass.

### 3.3 Failure modes to handle

- `pactl` not installed → log + fall back to Path A (or
  half-duplex if Path A also fails).
- `module-echo-cancel` not available (rare; some distros split
  PipeWire packages) → same fallback.
- Module load takes seconds → probe the new source name before
  proceeding; supervisor's `/health` probe handles this naturally.

## Step 4 — Re-run M6 acceptance gates 1, 3, 4

With either Path A working or Path B engaged:

### 4.1 Gate 4 — ERLE on validation fixture

`acva demo aec-hw` with the chirp + pink noise stimulus. Pass:
ERLE > 25 dB after 4 s of convergence. Record per-second table
in the report.

### 4.2 Gate 1 — TTS doesn't trigger VAD

Set `cfg.audio.half_duplex_while_speaking: false` (the gate that
masked the failure pre-M6B). Run `acva` with continuous TTS for
30 minutes. Pass: `voice_vad_false_starts_total / 30` < 1.

### 4.3 Gate 3 — barge-in audibility

While TTS plays, speak a short phrase. The realtime STT should
publish a `FinalTranscript` containing the user's phrase, not the
TTS output. Pass: 5 / 5 attempts produce a clean transcript of
what the user said.

(Note: barge-in proper — the FSM transitioning out of `Speaking`
on `UserInterrupted` — is M7's job. Gate 3 here only verifies the
mic-side signal is clean enough for a discriminator to be
buildable. M7 builds the discriminator.)

## Step 5 — VAD threshold re-baseline (M6 Step 7 carryover)

If working AEC reveals that VAD's `onset_threshold = 0.5` /
`offset_threshold = 0.35` need adjustment on the AEC-cleaned
signal, document the new defaults in
`plans/open_questions.md` section L. Most likely outcome: the
defaults survive — Silero's training data already includes
AEC-style cleaned audio.

## Step 6 — Update production defaults + docs

Based on the path that passed:

- `config/default.yaml` — flip `apm.use_system_aec` /
  `apm.aec_enabled` / `audio.half_duplex_while_speaking`
  to the new recommended combination.
- `docs/aec_report.md` — append Step 1.1 + 1.2 results, the
  decision, and the final acceptance numbers.
- `docs/troubleshooting.md` — add a "no AEC" symptom section
  pointing at `acva demo aec-hw`.
- `plans/milestones/m6_aec.md` — flip gates 1, 3, 4 from
  "BLOCKED" / "FAIL" to PASS.

---

## Acceptance

1. M6 gates 1, 3, 4 all pass on at least one configuration of the
   dev workstation (in-process APM with USB mic OR PipeWire
   system-AEC OR both).
2. `config/default.yaml` ships a configuration that works on the
   default dev workstation hardware out of the box.
3. With that default config, `acva` running with continuous TTS
   for 30 minutes produces zero phantom STT transcripts of its own
   output (visible in the per-run log file —
   `/var/log/acva/acva-*.log` — by absence of `final_transcript`
   events that match the assistant's prior `llm_sentence` events).

## Risks

| Risk | Mitigation |
|---|---|
| USB mic isn't available immediately | Path B (system AEC) is the contingency; works without new hardware. |
| PipeWire's `module-echo-cancel` doesn't help on this codec | Then we ship the half-duplex gate as the documented production default for this hardware class and proceed to M7 with M7 explicitly aware that barge-in won't be available without external hardware. |
| `pactl load-module` interferes with the user's existing audio config | Shutdown handler unloads our module by ID. Document the recovery (`pactl unload-module N`) for hangs. |
| Path B becomes the default and the production config gets coupled to PipeWire | `cfg.apm.use_system_aec: false` on systems without PipeWire; orchestrator detects and falls back. Ship `cfg.apm.aec_enabled: true` as the cross-platform path. |

---

## Time breakdown

| Step | Cost |
|---|---:|
| 1   `acva demo aec-record` + `scripts/aec_analyze.py` | 2.5 d |
| 2.1 lower-amp retest (re-uses Step 1 tools) | 0.5 d |
| 2.2 USB-mic retest (assuming hardware on hand) | 0.5 d |
| 3   PipeWire fallback wiring + tests | 1.5 d |
| 4   Acceptance gate re-runs | 1 d |
| 5   VAD re-baseline if needed | 0.5 d |
| 6   Doc + config updates | 0.5 d |
| **Total** | **~7 d** |

The recording demo (Step 1) adds ~2 days vs the original ad-hoc
plan but pays back across Steps 2–4: every probe re-uses the same
WAV-capture tooling, so re-analyzing a new configuration is one
demo run + one script invocation rather than rebuilding C++.

Path A-only outcome (Step 2.2 passes) shaves Step 3 entirely —
~5.5 days.
