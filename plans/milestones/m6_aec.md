# M6 — AEC / NS / AGC

**Estimate:** 1–2 weeks.

**Depends on:** M3 (playback engine; loopback tap point), M4 (capture path; resampler).

**Blocks:** M7 (barge-in needs AEC to avoid TTS-echo false triggers).

## Goal

Speaker-mode operation works without the assistant interrupting itself. WebRTC APM consumes mic + reference signal frames; emits cleaned audio that VAD operates on.

The single most-overlooked component in voice-agent design is the **reference-signal routing**: the loopback tap point determines whether AEC succeeds or fails. We tap **after** the playback resampler and **before** the audio device — that is, what the device actually emits.

## Out of scope

- Post-MVP signal-quality refinements (deeper noise suppression, adaptive AGC tuning).
- Multi-microphone setups.

## New deps

| Lib | Source | Purpose |
|---|---|---|
| WebRTC APM | vendored | AEC, NS, AGC |

WebRTC APM isn't packaged on Manjaro or Debian. Two approaches:

**A.** Vendor as a CMake submodule (`third_party/webrtc-apm`) — pinned commit from a maintained fork (`pulseaudio/webrtc-audio-processing` is the canonical Linux fork).
**B.** Use Arch's `webrtc-audio-processing-1` package — does it expose a stable CMake config? If yes, prefer this.

```bash
pacman -Si webrtc-audio-processing-1
```

If packaged config is usable, prefer A; else vendor.

## Step 1 — Loopback tap

The playback engine in M3 emits int16 samples at 48 kHz to the device. Tap *just before* `Pa_WriteStream` (or whatever PortAudio call) and copy into a per-utterance loopback ring with timestamps.

**Files:**
- Modify `src/playback/engine.hpp/cpp`: add `LoopbackSink` interface.
- `src/audio/loopback.hpp`, `src/audio/loopback.cpp`.

```cpp
class LoopbackSink {
public:
    // Called from the playback engine on every chunk it emits to the device.
    // Stores the chunk into a ring buffer indexed by capture-frame timestamp.
    void on_emitted(std::span<const std::int16_t> samples_48k,
                    std::chrono::steady_clock::time_point emit_time);

    // Pull frames aligned to a capture timestamp + duration. Returns the
    // matched samples (and zero-pads if loopback hasn't been emitted yet).
    std::vector<std::int16_t> aligned(std::chrono::steady_clock::time_point capture_time,
                                      std::size_t samples_needed);
};
```

The `MonotonicAudioClock` from M4 keeps both streams on the same time base.

## Step 2 — APM wrapper

**Files:**
- `src/audio/apm.hpp`
- `src/audio/apm.cpp`

WebRTC APM consumes 10-ms frames. The wrapper:
1. Receives mic frames at 16 kHz mono.
2. Pulls aligned reference frames from `LoopbackSink` (48 kHz → resample to 16 kHz on the fly).
3. Feeds both into `webrtc::AudioProcessing::ProcessReverseStream` (reference) and `ProcessStream` (mic).
4. Reads back the cleaned mic frame.
5. Reports AEC delay estimate, ERLE, residual echo metrics.

```cpp
struct ApmConfig {
    bool aec_enabled = true;
    bool ns_enabled = true;
    bool agc_enabled = true;
    int reverse_sample_rate_hz = 16000;
    int near_sample_rate_hz = 16000;
    int max_aec_delay_ms = 250;
};

class Apm {
public:
    Apm(ApmConfig cfg);
    ~Apm();

    // Process a 10 ms mic frame. Returns the cleaned frame. Pulls the
    // aligned reference internally from the LoopbackSink.
    std::vector<std::int16_t> process(std::span<const std::int16_t> mic_16k_10ms,
                                       std::chrono::steady_clock::time_point capture_time);

    [[nodiscard]] int aec_delay_estimate_ms() const noexcept;
    [[nodiscard]] float erle_db() const noexcept;
};
```

## Step 3 — Integrate into the audio-processing thread

Today (M4) the audio-processing thread is:
```
SPSC ring → Resample → VAD → Endpointer
```
M6 inserts:
```
SPSC ring → Resample → APM (mic+ref) → VAD → Endpointer
```

VAD operates on the AEC-cleaned signal. The endpointer's thresholds may need slight tuning post-APM (the noise floor changes), but the same defaults usually work.

## Step 4 — Reference-signal alignment delay

WebRTC APM expects the reference signal to **arrive before** the corresponding mic signal — i.e., the speaker output must be fed in advance of when the microphone hears it. The native delay is the **device output latency + airborne propagation + microphone latency**. On a typical desktop with USB mic and built-in speakers, this is 30–80 ms.

The `LoopbackSink` stores frames with their emit timestamps. When the APM asks for the reference at `capture_time`, the aligned sample is `loopback[capture_time - estimated_delay]`. APM has built-in delay estimation that converges over a few seconds; we feed it an initial guess of 50 ms and let it adjust.

**Metric:** `voice_aec_delay_estimate_ms` exposed via `Apm::aec_delay_estimate_ms()`. Watch this during dev — if it diverges over time, drift correction in `MonotonicAudioClock` isn't keeping up.

## Step 5 — Echo-suppression validation

A standalone test that exercises the end-to-end loop:
1. Play a known signal (1 kHz sine) through the speaker.
2. Capture mic input.
3. Run through APM.
4. Measure ERLE (echo return loss enhancement) on the AEC output: should be > 25 dB after convergence.

**Files:** `tests/test_aec_validation.cpp` — gated, requires real audio hardware. CI skips it (we have no CI anyway, but document the gate).

## Step 6 — Config extension

```yaml
apm:
  aec_enabled: true
  ns_enabled: true
  agc_enabled: true
  initial_delay_estimate_ms: 50
  max_delay_ms: 250

audio:
  loopback:
    ring_seconds: 2          # how much loopback history to retain
```

## Step 7 — Re-baseline VAD thresholds

After APM is in place, re-run the M4 false-start fixtures with the cleaned signal. Tune `vad.onset_threshold` and `vad.offset_threshold` if needed. Document any change in `plans/open_questions.md` as a tuning note (no new question; just an outcome).

## Test plan

| Test | Scope |
|---|---|
| `test_loopback.cpp` | aligned sample retrieval; ring overflow behavior |
| `test_apm.cpp` | wrapper round-trip; cleaned-output non-empty; metrics export |
| `test_aec_validation.cpp` (gated) | real-audio ERLE measurement |
| Manual integration | speak with assistant playing music nearby; verify VAD doesn't trigger on the music or the assistant's own voice |

## Acceptance

1. With speakers on (no headphones), TTS playback does not trigger VAD `SpeechStarted`. `voice_vad_false_starts_total` increases by < 1 per minute of TTS playback.
2. AEC delay estimate stabilizes within 3 seconds and stays within ±10 ms of its converged value over a 30-minute soak.
3. Speaking *over* TTS playback still produces correct VAD endpointing within the M4 acceptance criteria — i.e., the user's voice is preserved while the assistant's voice is cancelled.
4. ERLE > 25 dB on the validation fixture after convergence.
5. `voice_aec_delay_estimate_ms` exposed and updates.

## Risks specific to M6

| Risk | Mitigation |
|---|---|
| Loopback alignment off by frames → AEC fails | Single `MonotonicAudioClock`; explicit delay-estimate metric; validation fixture |
| Delay estimate drifts over hours | Drift correction in clock; soak test verifies P95 |
| WebRTC APM packaging | Either system pkg (preferred on Arch) or vendored fork; document both paths |
| AGC clobbers user's natural dynamic range | Make AGC opt-out; default on; document |
| AEC can suppress real user speech if reference signal is wrong | Validation fixture catches; tuning iterates |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Loopback tap | 1.5 days |
| 2 APM wrapper | 2 days |
| 3 Integration | 1 day |
| 4 Delay alignment | 1.5 days |
| 5 Echo validation | 1 day |
| 6 Config + thresholds | 0.5 day |
| 7 VAD re-baseline | 1 day |
| **Total** | **~8–9 days = ~1.5–2 weeks** |
