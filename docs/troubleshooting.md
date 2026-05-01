# Troubleshooting

Symptom-first guide. Each section starts with the user-visible
problem, then walks down the chain using `acva demo <name>` commands.
Every demo prints a final `demo[<name>] done: …` line on success or
`demo[<name>] FAIL: …` on failure with the immediate cause — those
lines are the canonical diagnostic.

> **Prerequisite for everything below:** a working `./build.sh` and
> the compose stack (`docker compose -f packaging/compose/docker-compose.yml ps`)
> showing `llama` / `whisper` / `piper` healthy. If you're below that
> bar, fix that first — none of the demos will help.

## Quick triage

```sh
./_build/dev/acva demo health    # M2 — backends + voice synth
./_build/dev/acva demo tone      # M3 — audio output (no Piper)
./_build/dev/acva demo tts       # M3 — Piper + playback
./_build/dev/acva demo llm       # M1 — LLM streaming
./_build/dev/acva demo chat      # M1+M3 — text-in → speech-out
./_build/dev/acva demo fsm       # M0 — FSM, no backends
```

The first failing demo points at the broken hop.

## By symptom

### "I hear nothing"

Three steps, smallest scope first:

1. **`acva demo tone`** — does a 1.5 s 440 Hz sine play?

   - **Plays** → audio device is fine; problem is upstream (Piper
     wiring or the dialogue chain). Skip to step 2.
   - **Silent + log says `headless mode running`** → PortAudio
     couldn't open the device. See the *Audio device* section below.
   - **Silent + log says `PortAudio stream open`** but you still hear
     nothing → the device opened but routes audio somewhere unexpected
     (often HDMI when you expect analog). Run `pavucontrol` during
     the demo and check the Playback tab — an "acva" stream should
     appear and not be muted.

2. **`acva demo tts`** — does Piper synthesize "Hello from acva."?

   - **You hear it** → M3 path works. The `--stdin` chain or full
     `acva` run is misconfigured. Re-check `cfg.tts.voices`.
   - **Silent + `first_audio_ms=-1`** → bridge couldn't reach Piper.
     Run `acva demo health` next.
   - **`FAIL: cfg.tts.voices is empty`** → populate that map. Default
     is `en: { url: "http://127.0.0.1:8083" }` matching the Piper
     voice that `packaging/compose/fetch-assets.sh` downloads.

3. If both pass and it's only `acva --stdin` that's silent: confirm
   the startup log says `tts enabled — voices configured: N` (not
   `tts disabled`). Check that the `LlmStarted`-driven active-turn
   override in main.cpp is wired (see the M3.8 commit if uncertain).

### "No LLM reply"

**`acva demo llm`** sends a fixed prompt directly to llama:

| Output | Likely cause |
|---|---|
| `FAIL: /health probe failed at http://127.0.0.1:8081/v1` | llama isn't listening. `docker compose ps llama` → `up -d`. |
| `/health ok; submitting prompt: …` then `FAIL: zero tokens received` | Model alias mismatch. Check `cfg.llm.model` against the `--alias` flag in `docker-compose.yml`. |
| Tokens stream cleanly | M1 path is fine; the symptom is further down. Try `acva demo chat`. |

For chat-specific failures (LLM streams in `demo llm` but `demo chat`
hangs), see *Compose health* below — usually one container went
unhealthy mid-run.

### "Backend says it's down"

**`acva demo health`** probes everything once:

```
demo[health] probing 3 service(s) with timeout=3000ms:

  llm      http://127.0.0.1:8081/health   ok=yes  http=200  latency=  1ms
  stt      http://127.0.0.1:8082/health   ok=yes  http=200  latency=  0ms
  tts      <not configured>               ok=no   http=0    latency=  0ms
  tts/en   http://127.0.0.1:8083          ok=yes  http=200  latency= 62ms
```

| Failure row | Likely cause |
|---|---|
| `ok=no http=0 err="probe: Connection"` | Container not bound to the configured port. `docker compose ps`. |
| `ok=no http=503` | Service started but not ready (model still loading). Wait + re-run. |
| `tts/en ok=no http=500` | Piper reached but rejected the synth probe. Usually a missing voice file. `docker compose logs piper`. |
| `tts <not configured>` | Expected — Piper has no `/health` route. The `tts/<lang>` rows below it are the real check. |
| All green but `acva demo chat` still fails | Race or lifecycle issue. Look at `/status`'s `pipeline_state` next. |

### "/status reports `pipeline_state: failed`"

Supervisor flipped a critical service to Unhealthy past
`cfg.supervisor.pipeline_fail_grace_seconds`. The dialogue path is
gated until recovery.

```sh
curl -s http://127.0.0.1:9876/status | jq '.services'
```

The service with `state: "unhealthy"` is the culprit — `docker compose
logs <service>` for the cause. Once the backend is back, the
supervisor recovers within `probe_interval_degraded_ms × 2` and
`pipeline_state` flips back to `ok` automatically.

### "Audio is choppy / underruns climbing"

```sh
curl -s http://127.0.0.1:9876/metrics | \
    grep -E 'voice_playback_(underruns|drops|queue_depth)'
```

| Pattern | Diagnosis |
|---|---|
| High underruns + low queue_depth | Producer can't keep up. Check `voice_tts_first_audio_ms` distribution — Piper synth is slow. |
| High drops + queue_depth at cap | Producer too fast OR consumer too slow. Increase `playback.max_queue_chunks`, or check for `headless mode running` in stderr. |
| Both ≈ 0 but still choppy | Device-level: sample-rate mismatch or ALSA buffer too small. Check stderr for ALSA `error.pcm` lines (cosmetic ones from PortAudio enumeration are harmless; sustained xruns are not). |

### "Sentences are cut off mid-thought"

Look for `sentence cap hit: turn=N cap=M — cancelling LLM stream` in
the log. That means `cfg.dialogue.max_assistant_sentences` (default 6)
fired. Either:

- Increase the cap if the user wants longer replies, or
- Adjust the system prompt to ask for shorter answers (cheaper than
  paying for tokens that get truncated).

If you DON'T see the cap line and replies still end early, the LLM is
stopping itself — check `voice_llm_first_token_ms` and Compose logs
for OOM / token-limit hits.

### (M4+) "Mic not detected"

**`acva demo loopback`** is the smallest mic→speaker round-trip.

- `FAIL: input device not found` → name in `cfg.audio.input_device`
  doesn't match. `arecord -L` lists candidates; pick a substring of
  one and try again.
- `frames_captured << expected` → the audio thread is being preempted.
  Check CPU governor / thermal throttling.

If `loopback` works but **`acva demo capture`** reports `utterances=0`
during obvious speech, VAD thresholds are off:

- Lower `cfg.vad.onset_threshold` (default 0.5) toward 0.35 if you
  speak quietly.
- Raise `cfg.vad.offset_threshold` if utterances merge.

### (M5+) "Transcript is wrong"

**`acva demo stt --fixture FILE.wav`** removes the mic from the
equation. If the fixture transcribes correctly but live mic doesn't,
the audio path before STT is at fault — re-run `acva demo capture`
with the same words you spoke.

### (M7+) "Barge-in fires when it shouldn't"

**`acva demo bargein`** verifies the cancellation cascade in
isolation. If it passes but real barge-in misfires, the issue is
upstream of the cascade — usually AEC. **`acva demo aec`** measures
ERLE; below 25 dB means the assistant's own voice is leaking into the
mic and tripping VAD.

## Audio device

If `acva demo tone` reports `headless=true` or you can't hear the
sine even when the demo claims success:

```sh
# What devices does PortAudio see?
pactl list short sinks   # PulseAudio
aplay -L                 # ALSA
```

- **Headless mode entered** → PortAudio init failed. Common causes:
  - Running over SSH without `XDG_RUNTIME_DIR` / Pulse cookie.
  - `cfg.audio.output_device: "none"` (explicit headless).
- **Wrong device** → set `cfg.audio.output_device` to a *substring* of
  the device's friendly name. Match is case-insensitive. Empty string
  or `"default"` uses the host default.
- **Plays for `aplay /tmp/p.wav` but not for acva** → Pulse routed the
  acva stream to the wrong sink. Use `pavucontrol` Playback tab to
  move it.

The ALSA `Unknown PCM cards.pcm.rear / center_lfe / side` lines on
stderr are harmless — that's PortAudio enumerating non-existent
surround channels on a stereo card.

## Compose stack

```sh
docker compose -f packaging/compose/docker-compose.yml ps
docker compose -f packaging/compose/docker-compose.yml logs --tail=50 llama
```

Common boot failures:

| Service | Symptom | Fix |
|---|---|---|
| `llama` | `unable to load model` | Run `packaging/compose/fetch-assets.sh` to download the Qwen GGUF shards. Check `${ACVA_MODELS_DIR}` (default `~/.local/share/acva/models`). |
| `whisper` | `cannot load model from /models/ggml-small.bin` | Same — fetch-assets downloads it. |
| `piper` | `voice file not found` | Voice file not in `${ACVA_VOICES_DIR}`. fetch-assets puts `en_US-amy-medium.onnx` there. |
| All three | `nvidia-smi` not visible inside container | Install the NVIDIA Container Toolkit on the host; restart Docker. |

## Component → demo cross-reference

| Component | Demo | What's verified |
|---|---|---|
| Config loader | (any) | YAML parses, paths resolve via XDG |
| `EventBus` + `Fsm` | `fsm` | bus dispatch, FSM transitions, FakeDriver pump |
| `LlmClient` | `llm` | libcurl SSE, model alias, streaming |
| `HttpProbe` + `Supervisor` | `health` | `/health` GETs + voice POST probes |
| `Resampler` | `tone` | soxr 22.05↔48 (also smoke-tested in `tts`) |
| `PlaybackQueue` + `PlaybackEngine` | `tone` | mutex queue, audio cb, headless fallback |
| `PiperClient` + `TtsBridge` | `tts` | POST + WAV parse + resample + queue + engine |
| `Manager` | `chat` | PromptBuilder → LlmClient → SentenceSplitter → TtsBridge |
| (M4) Audio capture + VAD | `capture`, `loopback` | input device, SPSC ring, VAD endpointing |
| (M5) Streaming STT | `stt` | Whisper client, partials, language detection |
| (M6) AEC | `aec` | loopback tap, APM convergence, ERLE |
| (M7) Barge-in cascade | `bargein` | UserInterrupted propagation + persistence policy |
| (M8) Hardening | `soak`, `wipe`, `reload` | leak / hot-reload / privacy commands |

## When to file a bug

Open an issue (or attach to your branch) when:

- A demo reports `FAIL` and the diagnostic doesn't match anything
  above.
- The full test suite (`./run_tests.sh`) passes but a demo against
  real backends fails reproducibly. That gap usually means a
  bus-dispatch race or a config-default drift; capture the run with
  `logging.level: debug` and attach the stderr.
- `acva demo soak` (when M8 lands) reports growing RSS or
  monotonically rising queue depth — that's a leak.

## See also

- [`plans/milestones/README.md`](../plans/milestones/README.md) — what
  each milestone unlocks for the user, with concrete commands per
  milestone.
- [`plans/project_design.md`](../plans/project_design.md) §17 — the
  authoritative milestone roadmap.
- [`CLAUDE.md`](../CLAUDE.md) — repository layout + working notes.
