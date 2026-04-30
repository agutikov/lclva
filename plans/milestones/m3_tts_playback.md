# M3 — TTS + Playback

**Estimate:** ~1 week (down from 1-2 weeks; the Piper-wrapper subproject is gone).

**Depends on:** M1 (sentences flow as `LlmSentence` events; Compose stack from M1.B already runs `piper.http_server`), M2 (TTS service health-probed).

**Blocks:** M7 (barge-in needs a playback queue to drain).

## Goal

End-to-end **text-input → spoken-output**. The orchestrator sends each `LlmSentence` to Piper, queues the resulting audio chunks, and plays them through PortAudio. Per-language voice routing works: a turn detected as `lang="ru"` selects a Russian Piper voice. Cancellation by turn id is tested against simulated barge-in.

By the end of M3 you can talk to the assistant by typing, and it speaks back fluently in whichever language you asked.

## Out of scope

- Microphone input (M4).
- AEC reference signal — playback simply emits to the device; the loopback tap lands in M6.
- Audio device hot-plug detection.

## New deps

| Lib | Version | Purpose |
|---|---|---|
| portaudio | 19.7+ | playback |
| soxr | 0.1.3+ | resample 22.05 kHz Piper → 48 kHz device |

```cmake
pkg_check_modules(portaudio REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(soxr REQUIRED IMPORTED_TARGET soxr)
```

## Step 1 — Piper HTTP client

We talk to **upstream `python -m piper.http_server`**, already running in the Compose stack from M1.B. No custom wrapper. The upstream server's API is documented in `rhasspy/piper`:

- `POST /` body: raw text (or JSON `{"text": "..."}`), header `Content-Type: text/plain`. Response: WAV bytes (PCM, mono, 22.05 kHz int16).
- The voice is set per-process at startup via `--model` — there is no per-request voice selection.

**Implication for per-language voices.** Upstream `piper.http_server` runs *one* voice per server. So either:

- **Option A (default):** run multiple Compose services (one per language) on different ports. `piper-en` on 8083, `piper-ru` on 8084, etc. The `PiperClient` selects the URL by language. Adds 1 service per language to compose; ~30-100 MB RAM each.
- **Option B (post-MVP):** write a thin shim that loads multiple voices in one process and routes by `?voice=...`. Reverts to the original wrapper plan but only when proven necessary.

M3 ships option A; it's simpler and matches the "use upstream verbatim" decision. The voice→URL map lives in `cfg.tts.voices`.

**Files:**
- `src/tts/piper_client.hpp`
- `src/tts/piper_client.cpp`

(No `packaging/piper-server/` — it's deleted from the plan.)

**`PiperClient` API:**
```cpp
struct TtsRequest {
    dialogue::TurnId turn;
    event::SequenceNo seq;
    std::string text;
    std::string lang;                                       // selects voice
    std::shared_ptr<dialogue::CancellationToken> cancel;
};

struct TtsCallbacks {
    std::function<void(std::span<const std::int16_t> samples)> on_audio;
    std::function<void()> on_finished;       // called once per request
    std::function<void(std::string err)> on_error;
};

class PiperClient {
public:
    PiperClient(const Config& cfg);
    void submit(TtsRequest req, TtsCallbacks cb); // posts to I/O thread
    bool probe();
};
```

Each request runs to completion serially in M3 — Piper synthesis is sub-second per sentence and concurrent submission complicates voice loading. Reentrant in M5 if we need overlapping synthesis for speculation.

## Step 2 — Per-language voice routing (client-side)

Voice selection is done by URL: `cfg.tts.voices` maps `lang → http://...:port` and the `PiperClient` routes there. Each URL backs a separate Compose service with its own model file mounted in.

**Config:**
```yaml
tts:
  voices:
    en: { url: "http://127.0.0.1:8083" }   # piper-en service
    ru: { url: "http://127.0.0.1:8084" }   # piper-ru service
    de: { url: "http://127.0.0.1:8085" }   # piper-de service
  fallback_lang: en       # used when detected lang has no entry
  request_timeout_seconds: 10
```

When the user adds a language: drop a `piper-<lang>` service into `docker-compose.yml`, drop the voice file into `~/.local/share/lclva/voices/`, add a row to `cfg.tts.voices`. We ship `scripts/download-piper-voices.sh` for grabbing default voices.

There is no in-process voice memory budget anymore (it's now per-service RAM, governed by Compose). For workstations where this is too much, drop unused languages from compose.

## Step 3 — Playback queue

**Files:**
- `src/playback/queue.hpp`
- `src/playback/queue.cpp`

The playback queue is a bounded FIFO of audio chunks tagged `(turn_id, sequence_no)`. The PortAudio callback drains it.

```cpp
struct AudioChunk {
    dialogue::TurnId turn = 0;
    event::SequenceNo seq = 0;
    std::vector<std::int16_t> samples;   // 48 kHz mono
};

class PlaybackQueue {
public:
    explicit PlaybackQueue(std::size_t max_chunks);

    // Producer (TTS thread): enqueue. Rejects if turn != active turn.
    bool enqueue(AudioChunk chunk);

    // Consumer (audio callback): pop the next chunk for the active turn.
    // Returns nullopt if empty or only stale chunks present.
    std::optional<AudioChunk> dequeue_active(dialogue::TurnId active_turn);

    // Called on barge-in: invalidate everything for the previous turn,
    // emit PlaybackInterrupted via callback. Lock-free against the audio
    // thread by setting an atomic generation counter; the audio callback
    // reads it on each pop.
    void invalidate_before(dialogue::TurnId next_turn);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t drops() const noexcept;
};
```

The `dequeue_active` skips chunks where `turn != active_turn` (still on the queue from a cancelled turn). They get dropped and counted.

## Step 4 — Playback engine

**Files:**
- `src/playback/engine.hpp`
- `src/playback/engine.cpp`

PortAudio callback runs at the device sample rate (48 kHz). It pulls from the `PlaybackQueue`. Constraints:
- Never blocks.
- Never allocates on the realtime thread.
- Reads `active_turn` atomically.
- On underrun, emits silence (zero buffer) and increments `voice_playback_underruns_total`.

When a chunk's last sample is consumed, the engine publishes `PlaybackFinished{ turn, seq }` (via a lock-free postbox to a non-realtime thread that does the publish — the audio thread itself can't touch the bus mutex).

**Cross-thread postbox:** a tiny SPSC ring of `(turn, seq)` tuples between audio callback and a "playback bookkeeping" thread. Dedicated thread, low frequency.

## Step 5 — TTS resampler chain

Piper outputs 22.05 kHz mono int16. Playback device is 48 kHz mono int16. soxr resamples between them.

**File:** `src/audio/resampler.hpp`, `src/audio/resampler.cpp` — generic soxr wrapper. Reused in M4 for capture-side resampling (48 → 16).

```cpp
class Resampler {
public:
    Resampler(double in_rate, double out_rate, soxr_quality_spec_t quality);
    std::vector<std::int16_t> process(std::span<const std::int16_t> in);
    std::vector<std::int16_t> flush();
};
```

For Piper's 22.05 → 48: use `SOXR_HQ` quality; the asymmetric ratio is a pure soxr job, no special handling.

## Step 6 — Bridge: LlmSentence → TTS → Playback

A new component glues them:

**File:** `src/dialogue/tts_bridge.hpp`, `src/dialogue/tts_bridge.cpp`.

```cpp
class TtsBridge {
public:
    TtsBridge(event::EventBus& bus,
              tts::PiperClient& tts,
              audio::Resampler& resampler,
              playback::PlaybackQueue& queue);
    void start();
    void stop();
};
```

Subscribes to `LlmSentence`. For each:
1. Submit to PiperClient with the turn's cancellation token.
2. On `on_audio` chunks: resample 22.05 → 48, enqueue to PlaybackQueue, publish `TtsAudioChunk`.
3. On `on_finished`: publish `TtsFinished`.

Subscribes to `UserInterrupted` to invalidate the playback queue immediately (don't wait for the in-flight Piper request to finish).

## Step 7 — Sentence cap enforcement

Per the design (`dialogue.max_assistant_sentences = 6`): when the assistant has produced 6 sentences in a turn, the Dialogue Manager:
1. Cancels the LLM stream.
2. Lets the TTS/playback finish what's already queued.
3. Treats the resulting end-of-turn as `completed`.

This integrates with M1's manager. Just a counter check in the `LlmSentence` subscriber.

## Step 8 — Config extension

```yaml
audio:
  device:
    output: default          # default | "name" | index
  sample_rate_hz: 48000
  buffer_frames: 480         # 10 ms @ 48 kHz

tts:
  unit: "lclva-piper.service"
  base_url: "http://127.0.0.1:8082"
  voices: { en: ..., ru: ..., de: ... }
  voice_memory_budget_mb: 500
  voice_models_dir: ".../voices"

playback:
  max_queue_chunks: 64       # ~640 ms at 10 ms chunks
  underrun_log_throttle_ms: 1000

dialogue:
  max_assistant_sentences: 6
  max_tts_queue_sentences: 3
```

## Test plan

| Test | Scope |
|---|---|
| `test_resampler.cpp` | round-trip 22.05 ↔ 48; zero-input flush; quality preserved (sinusoid energy) |
| `test_playback_queue.cpp` | enqueue/dequeue ordering; `invalidate_before` drops only earlier turns; drop counter |
| `test_piper_client.cpp` | fake server fixture; voice routing by lang; cancellation; error path |
| `test_tts_bridge.cpp` | LlmSentence in → audio chunks out via fakes; UserInterrupted drains queue |
| Real Piper smoke | gated; speaks "hello world" and exits |
| Manual end-to-end | type a question, hear the answer in the chosen voice |

## Acceptance

1. Type a multi-sentence question, hear all sentences spoken in order in the configured English voice.
2. With `lang="ru"` injected via test harness, hear the Russian voice.
3. Issue a fake `UserInterrupted` mid-playback; sound cuts off within 50 ms; `voice_turns_total{outcome="interrupted"}` increments; remaining queued audio chunks are dropped (not played).
4. Soak: 100 consecutive turns, no leaks (RSS within +20 MB of post-warmup baseline), no underruns above the noise floor.
5. With one Piper service stopped (e.g. `docker compose stop piper-en`): the M2 supervisor flags it unhealthy. The orchestrator either uses the fallback language or, if `fail_pipeline_if_down` is true on the only configured TTS, gates the dialogue path.
6. `voice_tts_first_audio_ms` histogram captures latency.

## Risks specific to M3

| Risk | Mitigation |
|---|---|
| Per-language Compose service sprawl on a multilingual setup | Document; Option B (single shim with multi-voice routing) is the post-MVP escape hatch |
| Container memory cost (each Piper voice ~30-100 MB resident) | Drop unused languages from compose; document RAM expectations |
| PortAudio callback realtime constraints | Strictly no allocation/IO; cross-thread postbox for events; tested under heavy load |
| TTS request cancellation racing playback | Sequence-no enqueue/dequeue gate (already designed); covered by tests |
| Piper voice mismatches with detected language | Fallback to `cfg.tts.fallback_lang` when not in voices map; emit warning |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Piper HTTP client (no wrapper to write) | 0.5 day |
| 2 Per-language URL routing | 0.5 day |
| 3 Playback queue | 1 day |
| 4 Playback engine + PortAudio | 2 days |
| 5 Resampler | 0.5 day |
| 6 TTS bridge | 1 day |
| 7 Sentence cap | 0.5 day |
| 8 Config + tests | 1 day |
| **Total** | **~7 days = ~1 week** |
