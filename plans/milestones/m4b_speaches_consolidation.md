# M4B — Voice-Backend Consolidation onto Speaches

**Status:** ✅ landed. All 7 steps + the Step 0 smoke gate complete on the dev workstation. Compose is `llama` + `speaches` only; `OpenAiTtsClient` + `OpenAiSttClient` ship; `acva demo {tts,chat,stt}` exercise the new wiring; 207 unit + 8 integration tests green. One known follow-up captured: streaming-TTS playback jitter (M4B follow-up task) — pre-buffer threshold in `PlaybackEngine` lands before M5 starts.

**Estimate:** 4–6 days.

**Depends on:** M4 landed (this is a refactor of the existing
M3 TTS path; it doesn't touch capture/VAD).

**Blocks:** M5. M5 picks up the streaming-STT work *against the
Speaches realtime API* directly — no separate `WhisperClient` design
phase, no L1 (A/B/C) decision matrix, no custom Whisper service.

## Goal

Replace two upstream service containers (`whisper.cpp/server` and
`piper.http_server`) with a **single Speaches** container that fronts
both STT (faster-whisper) and TTS (Piper voices) behind one
OpenAI-compatible HTTP surface — while keeping the M0–M4 stack
provably working at every step.

The migration is **strict additive-then-deprecate**: bring up Speaches
alongside the existing services, prove parity with smoke tests, swap
the orchestrator's clients over one at a time, *then* delete the
obsolete infra. No "big-bang" cutover.

## Why

A late callout caught it: Speaches packages STT + TTS together and
explicitly positions itself as "Ollama for speech." The original M1/M3
plans chose `piper.http_server` and `whisper.cpp/server` independently
because they're the lowest-level shipping options — but that decision
predates Speaches' maturity. Adopting Speaches now:

1. **Drops the Compose surface from 3 services to 2** (`llama` +
   `speaches`). Operationally simpler; one fewer thing to babysit.
2. **Single OpenAI-compatible client surface** for both STT and TTS,
   matching the LLM client style. Removes the 1-off `PiperClient`
   payload format.
3. **Honors CLAUDE.md pillar #5** ("don't write a custom HTTP wrapper
   around a backend that already ships one"). Speaches *is* the
   already-shipped wrapper for streaming Whisper.
4. **Resolves M5 L1** before M5 starts — no decision-matrix scramble at
   the start of M5 because the streaming engine is already chosen and
   smoke-tested.
5. **Faster TTS migration than swapping STT** — TTS is sync HTTP, our
   M3 tests use an in-process httplib fixture, the swap is a payload
   reshape; doing it now de-risks M5's STT swap.

The risks of *not* doing M4B:
- M5 starts with a parallel "evaluate Speaches" task whose outcome
  retroactively wants to rewrite M3 work anyway.
- Two TTS code paths (Piper-direct + Speaches-via-OpenAI) might end up
  living in tree side-by-side longer than necessary.

## Out of scope

- **Streaming STT.** That's M5. M4B replaces the *existing
  request/response* whisper path with Speaches' equivalent endpoint;
  no `PartialTranscript` events yet. Streaming partials are the
  headline feature of M5.
- **Kokoro voices.** Speaches can also serve Kokoro TTS; we stick with
  Piper voices for parity with the M3 acceptance set.
- **Removing systemd packaging.** `packaging/systemd/` continues to
  describe a per-service production deployment; it gets one fewer unit
  file once `piper`/`whisper` are dropped, but that's wording-only.

## Verification gate (Step 0)

The 30-minute smoke that has to pass *before any code change*:

1. `docker pull ghcr.io/speaches-ai/speaches:latest-cuda`. Verify the
   image runs and `GET /health` answers 200 on the workstation
   (RTX 4060, 8 GB).
2. `curl -s POST /v1/audio/speech` with one of our existing Piper
   voice files (`en_US-amy-medium`). Confirm the response is valid
   PCM/WAV/Opus.
3. `curl -s POST /v1/audio/transcriptions` with a 3-second WAV.
   Confirm a sensible `text` field comes back.
4. Inspect Speaches' Piper voice loader: does it consume our existing
   `~/.local/share/acva/voices/*.onnx` files, or does it want a
   different layout? Document the layout in this plan and update the
   downloader script accordingly.
5. Skim `speaches-ai/speaches` recent commits — anything ≤ 60 days
   old, no abandonment signal.

If any of (1)–(5) fail, **abandon M4B**, stay on the current stack,
and revisit option A (custom whisper.cpp wrapper) at M5 start.

---

## Step 1 ✅ — Compose: add `speaches` alongside existing services

**Files:**
- `packaging/compose/docker-compose.yml`

Add a third (then-fourth) service block. **Do not remove `whisper` or
`piper` yet** — additive only. Pick a non-conflicting host port
(probably 127.0.0.1:8090) so neither existing service moves.

```yaml
speaches:
  image: ghcr.io/speaches-ai/speaches:latest-cuda
  container_name: acva-speaches
  ports: ["127.0.0.1:8090:8000"]
  volumes:
    # Speaches owns its own HF cache; we bind-mount it to a host
    # subdir so models survive container restarts. Layout inside the
    # cache is HuggingFace-standard (`models--<owner>--<repo>/`).
    - ${ACVA_MODELS_DIR:-${HOME}/.local/share/acva/models}/speaches:/home/ubuntu/.cache/huggingface/hub
  deploy:
    resources:
      reservations:
        devices:
          - driver: nvidia
            count: 1
            capabilities: [gpu]
  healthcheck:
    test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8000/health"]
  restart: unless-stopped
```

After Step 1: `docker compose up -d` brings up four containers. Existing
M3/M4 paths keep running through `whisper`/`piper`. The orchestrator
hasn't changed.

## Step 2 ✅ — Asset downloader for Speaches

**Files:**
- `scripts/download-speaches-models.sh` (new)
- `scripts/download-backend-assets.sh` (was `packaging/compose/fetch-assets.sh`; moved to `scripts/` during M4B for one-canonical-location ergonomics — sibling of `download-silero-vad.sh`. The whisper.cpp + Piper rows in it become unused after Step 6.)

`download-backend-assets.sh` already pulls llama, whisper, and piper assets.
`download-speaches-models.sh` is much simpler than originally drafted
because **Speaches owns its own model installation** via a `POST
/v1/models/{id}` endpoint (idempotent — returns 200 if already cached,
201 on first download). The script just hits Speaches and lets it pull
from HuggingFace into the bind-mounted cache directory.

The script:

1. Creates `${XDG_DATA_HOME}/acva/models/speaches/` if missing (the
   bind-mount target). No layout work — Speaches writes
   `models--speaches-ai--piper-{locale}-{voice}/` and
   `models--Systran--faster-whisper-{size}/` directories itself.
2. Waits for `GET /health` 200 with a generous timeout (Speaches takes
   ~3 s to start cold).
3. Loops over a small fixed list of model IDs (currently
   `Systran/faster-whisper-large-v3` and the Piper voices we ship in
   `config/default.yaml`'s `tts.voices`) and calls
   `POST /v1/models/{id}` for each. The endpoint is idempotent.
4. Verifies via `GET /v1/models` that everything in the install list
   shows up.
5. Same contract as `scripts/download-silero-vad.sh`: succeeds on a
   clean machine with no env vars set.

Side benefit: the script can also drive `acva demo health` (post-M4B)
to verify the install loop without needing a separate runbook. We can
copy the existing Piper `.onnx` files we already have under
`~/.local/share/acva/voices/` into the trash; Speaches' registry
mirrors them.

## Step 3 ✅ — Smoke tests against the running stack

**Files:**
- `packaging/compose/SMOKE.md` (new) — manual checklist
- `tests/test_speaches_smoke.cpp` (new, in the **integration** suite)

The integration test exercises the live Speaches container the same
way `test_vad.cpp` exercises the Silero model on disk. Cases:

1. `GET /health` answers 200 (probe sanity; gates the rest).
2. `POST /v1/audio/speech` with our existing en/ru Piper voices. Assert
   non-empty audio response and a recognizable PCM/WAV header.
3. `POST /v1/audio/transcriptions` with a fixture WAV and assert the
   transcript matches a known string (e.g., a Common Voice clip
   shipped under `tests/fixtures/audio/`).
4. Round-trip latency for both endpoints — assert under a generous
   threshold (e.g. STT < 5 s, TTS < 2 s on the workstation; tighten
   later in M8 soak work).

Tests `* doctest::skip` cleanly when the container isn't running so a
teammate without Compose up still gets a passing run. The dev env (per
the project memory note) has the stack running, so no skip there.

## Step 4 ✅ — Adapt the orchestrator: TTS first

**Files:**
- `src/tts/openai_tts_client.cpp` (new)
- `src/tts/openai_tts_client.hpp` (new)
- `src/tts/piper_client.cpp` (kept, marked deprecated in header)
- `src/tts/piper_client.hpp` (kept)
- `src/dialogue/tts_bridge.cpp` (rewire to use streaming receive)
- `src/config/config.hpp` (extend `TtsConfig`)
- `config/default.yaml` (per-language `voice_id` instead of `url`)
- `tests/test_openai_tts_client.cpp` (new — mirrors `test_piper_client.cpp`'s
  in-process httplib fixture; old test stays put, both link)

`OpenAiTtsClient` exposes the same interface as `PiperClient` (synthesize
one sentence in one language → stream of PCM samples) but talks
`POST /v1/audio/speech` against Speaches instead of one
`piper.http_server` per language. Voice routing collapses from
"language → URL" to "language → voice_id" against a single base URL.

**Use `response_format=pcm`, not `wav`.** The Step 0 smoke found Speaches'
WAV variant has a streaming-broken header (reports the first chunk's
size in the `data_size` field; a naive parser would truncate to ~1.4 s).
PCM dodges that entirely — bare int16 mono 22050 Hz stream, drops
straight into the existing `PlaybackQueue` after the M3 22050 → 48 kHz
resample.

**Consume the response as a stream, not a single buffer.** This is the
load-bearing performance change: Speaches' median TTFB is **10 ms**
versus Piper bare's **600 ms**, but only realised if the client starts
forwarding bytes to the `PlaybackQueue` as they arrive. The current
M3 `TtsBridge` uses cpp-httplib with a single response body; M4B
swaps that for a chunk-receiver callback (cpp-httplib supports it via
`Client::Post(..., ContentReceiver)`) or libcurl write-callback. Either
works; cpp-httplib keeps us off a second HTTP library on the TTS path.

Config shape change:

```yaml
tts:
  base_url: "http://127.0.0.1:8090/v1"   # NEW
  voices:
    en: { voice_id: "en_US-amy-medium" }
    ru: { voice_id: "ru_RU-irina-medium" }
  fallback_lang: en
```

The `voices[*].url` field is dropped in favor of one top-level
`base_url`. Validators in `config.cpp` change accordingly. Backwards
compat for the old shape isn't worth carrying (no production deploys
yet).

`TtsBridge` currently constructs `PiperClient`; behind a config flag
(`cfg.tts.provider`, defaulting to `"speaches"`) it constructs
`OpenAiTtsClient` instead. The flag exists *only* until Step 6 deletes
the Piper code path — no carried debt.

After Step 4: `acva demo tts` and `acva demo chat` pass against the new
client; old M3 tests still link until Step 6.

## Step 5 ✅ — Adapt the orchestrator: STT (request/response)

**Files:**
- `src/stt/openai_stt_client.cpp` (new)
- `src/stt/openai_stt_client.hpp` (new)
- `src/main.cpp` (subscribe to `UtteranceReady`, drive the STT client)
- `src/dialogue/manager.cpp` (no change yet — still reads
  `FinalTranscript` off the bus)
- `tests/test_openai_stt_client.cpp` (new)

`OpenAiSttClient::transcribe(AudioSlice)` posts the slice to
`/v1/audio/transcriptions` and publishes a `FinalTranscript` event on
the bus. **No streaming yet**; partials are M5. The existing
`PartialTranscript` event variant stays in `event::Event` unused —
M5 will start emitting it.

Why ship STT in M4B at all if M5 will rewire it? Because:
- It removes the fake driver's `FinalTranscript` synthesis (the M4
  follow-up hole) and gives us an end-to-end voice loop *now*.
- M5 then becomes a focused "swap request/response for streaming"
  task, not a "build STT from scratch" task.
- The fixture / wiring done here (audio slice → POST → event) is
  reused verbatim by M5's streaming client.

The fake driver's `FinalTranscript` synthesis stays available behind
`cfg.pipeline.fake_driver_enabled` for headless tests / `acva demo
fsm`.

## Step 6 ✅ — Remove obsolete infra

After Steps 4 + 5 land and the new clients have been driven through a
day of bench testing, delete:

**Files removed:**
- `src/tts/piper_client.cpp`
- `src/tts/piper_client.hpp`
- `tests/test_piper_client.cpp`
- `packaging/compose/whisper/` (entire dir — Dockerfile + setup)
- `packaging/compose/piper/` (entire dir)

**Files modified:**
- `packaging/compose/docker-compose.yml` — drop `whisper` and `piper`
  service blocks; rebind `speaches` to its production port (8082 or
  similar).
- `scripts/download-backend-assets.sh` — drop the whisper/piper
  rows; rename to `download-llama-model.sh` (or merge into a single
  `download-models.sh` umbrella). Decide at Step 6 implementation
  time, not now.
- `config/default.yaml` — drop `cfg.tts.voices[*].url` if any traces
  remain; tighten the voice-id schema docs.
- `cmake/Dependencies.cmake` — no change (we never linked Piper as a
  dep; it was a separate container).
- `CLAUDE.md` — update repo-layout block (`packaging/compose/whisper/`,
  `packaging/compose/piper/` lines disappear) and the M3 status
  paragraph (Piper → Speaches).

After Step 6: only one runtime backend service besides llama, and only
one HTTP-client style in tree (OpenAI-compatible).

## Step 7 ✅ — Update demos + docs

**Files:**
- `src/demos/tts.cpp` — re-target to the new client. Demo behavior is
  unchanged from the user's perspective.
- `src/demos/chat.cpp` — unchanged in spirit; verify post-Step-4.
- `src/demos/fsm.cpp` — unchanged; the fake driver still works.
- `docs/troubleshooting.md` — replace "piper container down"
  troubleshooting with "speaches container down". One container to
  blame instead of two.
- New demo: `acva demo stt`. Drives `OpenAiSttClient` against a
  fixture WAV (or the most-recent capture demo's slice) and prints
  the transcript. Slot in the demo table in `milestones/README.md`.

## Config schema after M4B

```yaml
# llm — unchanged.
llm:
  base_url: "http://127.0.0.1:8081/v1"
  model: "qwen2.5-7b-instruct"
  ...

# stt — new section. Replaces synthetic FinalTranscript path.
stt:
  base_url: "http://127.0.0.1:8090/v1"
  model: "Systran/faster-whisper-large-v3"
  request_timeout_seconds: 30
  health:
    health_url: "http://127.0.0.1:8090/health"
    fail_pipeline_if_down: true
    ...

# tts — reshaped: per-language voice_id under one base_url.
tts:
  base_url: "http://127.0.0.1:8090/v1"
  voices:
    en: { voice_id: "en_US-amy-medium" }
    ru: { voice_id: "ru_RU-irina-medium" }
  fallback_lang: en
  request_timeout_seconds: 10
  health:
    health_url: "http://127.0.0.1:8090/health"
    fail_pipeline_if_down: false   # TTS down ≠ pipeline down
    ...
```

`stt.health` and `tts.health` may end up pointing at the same Speaches
endpoint — the supervisor still treats them as logically separate
services so `/status` reports them as such. `fail_pipeline_if_down`
flips for STT once M5 makes the dialogue path actually depend on it.

## Test plan

| Test | Suite | Scope |
|---|---|---|
| `test_openai_tts_client.cpp` | unit | request shape, response decode, cancellation, retries |
| `test_openai_stt_client.cpp` | unit | request shape, response decode, language detection on fixture |
| `test_speaches_smoke.cpp` | integration | live Speaches at 8090 — health + TTS round-trip + STT round-trip |
| Existing `test_piper_client.cpp` | unit | runs until Step 6 deletes it |
| Existing `test_tts_bridge.cpp` | unit | adjust the fake transport class to match the new client surface |

`acva_unit_tests` count moves up by 2 cases when Step 4 lands and back
down to roughly its current count after Step 6 (Piper tests retire as
`OpenAiTtsClient` tests come in).

## Acceptance

1. `docker compose up -d` brings up `llama` + `speaches` only — no
   whisper, no piper containers.
2. `acva demo tts` and `acva demo chat` produce **subjectively
   indistinguishable** audio output to a listener compared to pre-M4B.
   Speaches' Piper port is ~8% louder and ~340 ms shorter (tighter
   silence trim) per sentence than `piper.http_server` direct — same
   voice file, expected variation. A/B listen during acceptance; if a
   noticeable artifact shows up, match Speaches' settings or document
   the difference.
3. `acva demo stt` transcribes a fixture WAV correctly.
4. End-to-end voice loop: speak → VAD endpoint → STT (`FinalTranscript`)
   → LLM → TTS → speakers, **without** the fake driver synthesizing
   any of it.
5. `/status` shows `speaches: healthy`. Both `stt` and `tts` services
   register against the same URL; supervisor handles them
   independently.
6. `cfg.tts.voices` no longer has any `url` fields.
7. **`voice_tts_first_audio_ms` P50 drops to ≤ 50 ms (smoke baseline:
   10 ms median) compared to M3's ~600 ms with `piper.http_server`.**
   This is the headline win of M4B — Speaches streams audio as it's
   generated. If we measure a regression instead, the streaming
   consumer in `TtsBridge` (Step 4) isn't actually streaming.
8. `voice_stt_*` metric family ships (cumulative requests, latency,
   error counts).

## Risks specific to M4B

| Risk | Mitigation |
|---|---|
| Speaches' Piper TTS support is half-working / Kokoro-first | Step 0(2)–(4) gates the entire milestone; if Piper-via-Speaches doesn't match standalone Piper, abandon M4B |
| Speaches' first-token TTS latency higher than `piper.http_server` direct | Acceptance #7 measures it; if a regression shows up, document and either accept (operational simplicity wins) or pin Piper for one more milestone |
| Speaches GPU image fights llama-server for the RTX 4060's 8 GB | Test side-by-side with both running before deleting whisper/piper; whisper-large-v3 + qwen2.5-7b-instruct-q4 both running may exceed VRAM — fall back to whisper-medium or move STT to CPU |
| Speaches upstream stagnates between M4B and M8 | Pin a specific image digest in compose; track upstream activity in M8 hardening review |
| Config refactor breaks existing user setups | No production deploys yet, so this is one-time pain. Document the change in CHANGELOG-style line in CLAUDE.md status |

## Time breakdown

| Step | Estimate |
|---|---|
| 0 Smoke gate | 0.5 day |
| 1 Compose add Speaches | 0.5 day |
| 2 Downloader script | 0.5 day |
| 3 Integration smoke tests | 1 day |
| 4 OpenAI TTS client + bridge swap | 1.5 days |
| 5 OpenAI STT client + main wiring | 1.5 days |
| 6 Remove obsolete infra | 0.5 day |
| 7 Demos + docs | 0.5 day |
| **Total** | **~6 days** |

## Why not just do this in M5?

Because M5's headline is *streaming partials + speculation*, and
mixing in "swap two HTTP clients + delete two services" makes M5's
acceptance criteria ambiguous. M4B keeps the surfaces small:

- **M4B** = "same behavior, one fewer service, OpenAI-compatible
  surface."
- **M5** = "swap request/response STT for streaming partials; add
  speculation."

If M4B fails its smoke gate, M5 falls back to L1 option A (custom
wrapper) — exactly the original plan. Nothing burns down.
