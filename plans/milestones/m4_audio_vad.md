# M4 — Audio Capture + VAD

**Status:** ✅ landed. All steps below complete; 24 new tests (SPSC ring, endpointer, utterance buffer, VAD construction; 3 model-gated tests skipped). Compiles + links against system `onnxruntime::onnxruntime` 1.24.x. Demos `acva demo loopback` and `acva demo capture` ship.

**Estimate:** 1–2 weeks.

**Depends on:** M3 (resampler shared, playback engine working). Requires PortAudio already wired.

**Blocks:** M5 (utterance buffer feeds streaming STT), M6 (AEC needs both streams to align).

## Goal

Microphone audio enters the pipeline. PortAudio captures at 48 kHz, the audio thread feeds a lock-free SPSC ring, the audio-processing thread resamples to 16 kHz, runs Silero VAD, and assembles utterances into reference-counted `AudioSlice` buffers. `SpeechStarted` and `SpeechEnded` events fire on the bus with correct timing.

The orchestrator now hears the user — but doesn't yet know what they said (M5).

## Out of scope

- AEC reference signal — the loopback tap is added in M6. Until then, M4 works correctly only with **headphones** (no echo into the mic).
- STT — utterances stop at the buffer; `FinalTranscript` events still come from synthetic sources.

## New deps

| Lib | Version | Purpose |
|---|---|---|
| ONNX Runtime | 1.18+ | Silero VAD |
| Silero VAD model (ONNX) | latest | the actual VAD weights, ~2 MB |

```cmake
find_package(onnxruntime CONFIG QUIET)  # gates ACVA_HAVE_ONNXRUNTIME
```

ONNX Runtime is **optional** (graceful fallback): when not present, `audio/vad.cpp` compiles to a stub that throws on construction; the `AudioPipeline` catches that and proceeds without VAD (probability fixed at 0). Real installs use the system package.

The ONNX model file ships separately; downloaded via `scripts/download-vad.sh` to `${XDG_DATA_HOME}/acva/models/silero/silero_vad.onnx`.

## Step 1 — MonotonicAudioClock ✅

**Files:**
- `src/audio/clock.hpp`
- `src/audio/clock.cpp`

A single clock object whose tick is the capture frame counter. All audio frames (capture and playback) carry timestamps derived from this clock, so AEC reference alignment is well-defined.

```cpp
class MonotonicAudioClock {
public:
    void on_capture_frames(std::uint64_t frame_count, std::uint32_t sample_rate);
    [[nodiscard]] std::chrono::steady_clock::time_point steady_for(std::uint64_t frame_index) const;
    [[nodiscard]] std::uint64_t frames_at(std::chrono::steady_clock::time_point) const;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
};
```

The clock provides drift correction in M6 by comparing against `steady_clock` over windows of seconds.

## Step 2 — Lock-free SPSC ring ✅

**Files:**
- `src/audio/spsc_ring.hpp` (header-only template)
- `tests/test_spsc_ring.cpp`

Hand-rolled per the design decision. ~100 lines. Atomic head/tail, fixed capacity, one producer and one consumer, padded to avoid false sharing on cache lines.

```cpp
template <class T, std::size_t Capacity>
class SpscRing {
public:
    bool push(T value);                 // producer-only
    std::optional<T> pop();             // consumer-only
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;  // approximate
};
```

`Capacity` is a compile-time constant (typical: 256 frames of 480 samples each = 480 × 256 × 2 bytes = ~240 KB). Strong memory-ordering tests (`std::memory_order_acquire`/`release`) and a 2-thread stress test under tsan in the test fixture.

## Step 3 — Audio capture frontend ✅

**Files:**
- `src/audio/capture.hpp`
- `src/audio/capture.cpp`

PortAudio input stream callback runs at the device sample rate (48 kHz, mono). The callback:
1. Wraps the incoming buffer in an `AudioFrame{ frame_index, samples_view, captured_at }`.
2. Pushes onto the SPSC ring.
3. Returns. Never allocates, never logs, never touches the bus.

The audio-processing thread `pop`s frames, runs them through:
- `Resampler` (48 → 16 kHz mono int16) — reuses M3's soxr wrapper.
- VAD — see Step 4.
- Utterance buffer — see Step 5.

Frame size: 10 ms at 48 kHz = 480 samples. After resample: 10 ms at 16 kHz = 160 samples. Silero VAD operates on 32 ms windows by default; we feed it 16 ms worth (256 samples) and it works fine if we accumulate.

## Step 4 — Silero VAD wrapper ✅

**Files:**
- `src/audio/vad.hpp`
- `src/audio/vad.cpp`

Silero VAD is an ONNX model. Inputs: 16 kHz mono int16 / float32 samples in fixed-size frames. Output: probability of speech in [0, 1].

```cpp
class SileroVad {
public:
    SileroVad(const std::filesystem::path& model_path);
    ~SileroVad();

    // Feed a 32 ms (or shorter, accumulated internally) frame. Returns
    // probability of speech.
    float push_frame(std::span<const std::int16_t> samples);

    void reset();                        // reset state between sessions
};
```

## Step 5 — Endpoint detector ✅

**Files:**
- `src/audio/endpointer.hpp`
- `src/audio/endpointer.cpp`

State machine driven by VAD probability:

- `Quiet` → `Onset` when probability > `onset_threshold` for `min_speech_ms` (default 200 ms).
- `Speaking` → `Endpoint` when probability < `offset_threshold` for `hangover_ms` (default 600 ms).
- After `Endpoint`, drop the trailing silence and emit `SpeechEnded`.

```cpp
struct EndpointerConfig {
    float onset_threshold = 0.5F;
    float offset_threshold = 0.35F;
    std::chrono::milliseconds min_speech_ms{200};
    std::chrono::milliseconds hangover_ms{600};
    std::chrono::milliseconds pre_padding_ms{300};   // included in utterance
    std::chrono::milliseconds post_padding_ms{100};
};

class Endpointer {
public:
    Endpointer(EndpointerConfig cfg, event::EventBus& bus);

    enum class FrameOutcome { None, SpeechStarted, SpeechEnded };
    FrameOutcome on_frame(std::span<const std::int16_t> samples_16k,
                          float vad_probability,
                          std::chrono::steady_clock::time_point frame_time);
};
```

`SpeechStarted` is emitted with `pre_padding_ms` of audio prepended to the utterance buffer (we keep a rolling pre-buffer of 300 ms).

## Step 6 — Utterance buffer with reference counting ✅

**Files:**
- `src/audio/utterance.hpp`
- `src/audio/utterance.cpp`

`AudioSlice` is a reference-counted view over a single allocation per utterance. Multiple consumers (STT, optional disk recorder) hold `shared_ptr<AudioSlice>`; the buffer is freed when the last one drops.

```cpp
class AudioSlice {
public:
    AudioSlice(std::vector<std::int16_t> samples,
               std::uint32_t sample_rate,
               std::chrono::steady_clock::time_point started_at,
               std::chrono::steady_clock::time_point ended_at);

    [[nodiscard]] std::span<const std::int16_t> samples() const noexcept;
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;
    [[nodiscard]] std::chrono::milliseconds duration() const noexcept;
};

class UtteranceBuffer {
public:
    explicit UtteranceBuffer(std::size_t max_in_flight);

    void on_speech_started(std::chrono::steady_clock::time_point pre_pad_start);
    void append(std::span<const std::int16_t> samples_16k);
    std::shared_ptr<AudioSlice> on_speech_ended(std::chrono::steady_clock::time_point);

    [[nodiscard]] std::size_t in_flight() const noexcept;
};
```

On overflow (more than `max_in_flight` utterances), drop the oldest with a metric increment.

## Step 7 — Wire it all up ✅

The audio-processing thread (separate from the audio callback thread) drains the SPSC ring and runs:

```
SPSC ring → Resampler → SileroVad + Endpointer → UtteranceBuffer
                                  ↓
                            event bus: SpeechStarted/SpeechEnded
                                  ↓
                            STT (publishes events on the bus carrying the AudioSlice;
                                 M5 wires that)
```

For M4, when an utterance ends, publish `SpeechEnded` carrying the `AudioSlice` via a new bus event:

```cpp
struct UtteranceReady {
    dialogue::TurnId turn = 0;
    std::shared_ptr<AudioSlice> slice;
};
```

Add to the `Event` variant. Subscribers in M4 are dummy (test only); STT subscribes in M5.

## Step 8 — Disable the fake driver's speech stream ✅

When `cfg.audio.capture_enabled = true`, the fake driver no longer publishes `SpeechStarted`/`SpeechEnded`. Real VAD takes over. The fake driver still produces `FinalTranscript`/`LlmSentence` until M5 / M1 cover those.

## Step 9 — Config extension ✅

```yaml
audio:
  device:
    input: default
    output: default
  sample_rate_hz: 48000
  buffer_frames: 480

vad:
  provider: silero
  model_path: "${XDG_DATA_HOME:-~/.local/share}/acva/models/silero/silero_vad.onnx"
  onset_threshold: 0.5
  offset_threshold: 0.35
  min_speech_ms: 200
  hangover_ms: 600
  pre_padding_ms: 300
  post_padding_ms: 100

utterance:
  max_in_flight: 3
  max_duration_ms: 60000      # safety cap; cut off long monologues
```

## Test plan

| Test | Scope |
|---|---|
| `test_spsc_ring.cpp` | ordering + tsan stress |
| `test_resampler.cpp` (extends M3) | 48→16 path, impulse response check |
| `test_endpointer.cpp` | hangover behaviour; min-speech filter; pre/post padding |
| `test_utterance.cpp` | buffer ref-counting; max-in-flight overflow |
| `test_vad.cpp` (gated) | run silero against fixed clip, assert probability >0.9 on speech, <0.1 on silence |
| Manual integration | speak into mic, observe `SpeechStarted` and `SpeechEnded` log lines with realistic timing |

## Demo commands (planned)

Two new `acva demo <name>` subcommands ship with M4. Both build only the M4
input path; they exit cleanly without user input beyond "speak when prompted".

### `acva demo loopback` — mic → speakers passthrough

Smallest possible smoke for the new capture path. Resamples mic input
48 → 16 kHz then back 16 → 48 kHz and pushes it straight through the
M3 playback queue. No VAD, no STT — just verifies the input device,
SPSC ring, and resampler chain.

Expected output:

```
demo[loopback] capture device='default' input rate=48000Hz duration=5s
demo[loopback] speak now…
demo[loopback] done: frames_captured=240000 underruns=0 overruns=0
```

Failure modes:
- `input device not found` → set `cfg.audio.input_device` to a substring of an `arecord -L` entry.
- `frames_captured << expected` + `overruns > 0` → SPSC ring overflowing because the audio thread is starved; check CPU / nice level.

### `acva demo capture` — mic + VAD endpointing report

Captures for 5 seconds, runs Silero VAD on the cleaned (post-resample,
no AEC yet) audio, and reports each utterance's start/end timestamps
plus VAD probability summaries. Useful for tuning thresholds without
spinning up the LLM.

Expected output:

```
demo[capture] device='default' duration=5s vad onset>=0.50 offset<=0.35 hangover=600ms
demo[capture] speak when ready…
  utterance #1: 1.243s → 2.901s (1.66s, peak p=0.97, mean p=0.71)
  utterance #2: 3.612s → 4.420s (0.81s, peak p=0.92, mean p=0.62)
demo[capture] done: utterances=2 false_starts=0 max_rms=0.42
```

Failure modes:
- `utterances=0` despite obvious speech → VAD onset threshold too high; lower `cfg.vad.onset_threshold` and re-run.
- `false_starts > utterances` → background noise above the floor; raise `cfg.vad.onset_threshold`.

What neither demo covers: STT (M5), AEC (M6). `loopback` doesn't tell
you whether VAD endpointing is sensible — that's `capture`'s job.

## Acceptance

1. Speaking briefly into the mic produces exactly one `SpeechStarted` and one `SpeechEnded` event with `~300 ms` pre-padding included in the utterance.
2. False starts (background TV at low volume) are filtered out: `voice_vad_false_starts_total` increments instead.
3. Soak (10 minutes of intermittent speech): no audio underruns at the playback side, no SPSC ring overflows, `voice_queue_depth` for audio frames stays under 50% of capacity.
4. With `pipeline.fake_driver_enabled: false` and headphones plugged in, drive an end-to-end turn: speak → see VAD endpoint → wait for fake STT → hear synthetic LLM reply. (Real STT lands in M5.)
5. Endpoint timing P95 is within 100 ms of an external ground-truth measurement on a recorded fixture.

## Risks specific to M4

| Risk | Mitigation |
|---|---|
| Audio callback allocation | Strict no-allocation rule; ASan with `MALLOC_CHECK_=2` runs verify; profiler check on hot path |
| ONNX Runtime initialization cost | One-time at startup; acceptable |
| Silero VAD over-triggering on background music | Document; AEC (M6) substantially improves this; speaker-mode demands AEC |
| Drift between capture frame count and steady_clock | `MonotonicAudioClock` corrects in windows; verified by 1-hour soak comparing two clocks |
| Endpointer too aggressive / too lenient | Configurable; tune on LibriSpeech fixtures during M4 |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Audio clock | 0.5 day |
| 2 SPSC ring + tests | 1 day |
| 3 Capture | 1 day |
| 4 Silero wrapper | 1 day |
| 5 Endpointer | 1.5 days |
| 6 Utterance buffer | 1 day |
| 7 Wiring | 1 day |
| 8/9 Config + integration | 1 day |
| Soak + tuning | 1.5 days |
| **Total** | **~8–9 days = ~1.5–2 weeks** |

## What landed (implementation notes)

- New files in `src/audio/`: `clock.{hpp,cpp}` (MonotonicAudioClock), `spsc_ring.hpp` (header-only template, hard-coded 64-byte cache line — `std::hardware_destructive_interference_size` is gated behind `-Werror=interference-size`), `frame.hpp` (`AudioFrame` with fixed 1024-sample slot for the worst-case ALSA short buffer), `capture.{hpp,cpp}` (PortAudio input wrapper with headless fallback, mirrors PlaybackEngine's structure), `vad.{hpp,cpp}` (Silero v5 wrapper, optional ONNX Runtime via `ACVA_HAVE_ONNXRUNTIME`), `endpointer.{hpp,cpp}` (4-state machine with `FalseStart` outcome — design plan only had 3 outcomes), `utterance.{hpp,cpp}` (AudioSlice + UtteranceBuffer with weak_ptr-based in-flight tracking + `max_duration_ms` head-truncation), `pipeline.{hpp,cpp}` (the consumer worker thread).
- New event variant member `UtteranceReady{turn, slice}` carrying `std::shared_ptr<audio::AudioSlice>`. Forward-declared in `event.hpp` so the event header doesn't pull in audio/utterance.hpp.
- `cfg.audio` extended with `input_device` + `capture_enabled`; new top-level `cfg.vad` and `cfg.utterance` sections. Validators in `config.cpp` enforce `offset_threshold ≤ onset_threshold` and the timing > 0 invariants.
- Fake driver gains `suppress_speech_events` so the M4 capture path owns Speech*; main.cpp passes `cfg.audio.capture_enabled` through.
- Demos `acva demo loopback` (mic → speakers, no VAD) and `acva demo capture` (mic + VAD endpointing report) ship.
- Tests: `test_spsc_ring.cpp` (6 cases incl. SPSC stress), `test_endpointer.cpp` (9), `test_utterance.cpp` (8), `test_vad.cpp` (1 + 3 model-gated). Total +24 cases. Suite: 210/210 passing locally.
- `scripts/download-vad.sh` pulls the v5.1.2 ONNX model.

Open follow-ups (deferred, not blocking):
- Metrics: `voice_vad_false_starts_total`, `voice_audio_ring_depth`, `voice_utterance_drops_total` plumbing — currently exposed via getters on `AudioPipeline` / `UtteranceBuffer` but not yet wired into `metrics::Registry`. Acceptance #2 + #3 need these.
- AEC reference signal — M6.
