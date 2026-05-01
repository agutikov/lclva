# Milestone plans

Detailed per-milestone plans. The high-level table lives in `../project_design.md` §17; this directory has one file per milestone with concrete steps, file lists, APIs, tests, and acceptance criteria.

| #   | File | Status      |
|-----|------|-------------|
| M0  | `m0_skeleton.md` | ✅ landed   |
| M1  | `m1_llm_memory.md` | ✅ landed   |
| M2  | `m2_supervision.md` | ✅ landed   |
| M3  | `m3_tts_playback.md` | ✅ landed   |
| M4  | `m4_audio_vad.md` | ✅ landed   |
| M4B | `m4b_speaches_consolidation.md` | ✅ landed |
| M5  | `m5_streaming_stt.md` | next |
| M6  | `m6_aec.md` | planned     |
| M7  | `m7_barge_in.md` | planned     |
| M8  | `m8_hardening.md` | planned    |

## Demos — one-shot smoke checks built into the binary

Every landed milestone exposes a `acva demo <name>` subcommand that
exercises its headline deliverable end-to-end with no user input. Run
`acva demo` (no name) for the catalog:

| Demo       | Milestone   | What it verifies |
|------------|-------------|------------------|
| `fsm`      | M0          | synthetic FSM driver runs through 3 turns, outcome counters increment |
| `llm`      | M1          | `LlmClient` reaches llama-server, streams a fixed prompt's reply to stdout |
| `health`   | M2          | `HttpProbe` hits each configured backend's `/health`, prints `ok / status / latency` |
| `tone`     | M3          | playback engine renders a 1.5 s 440 Hz sine through `cfg.audio.output_device` (no TTS) |
| `tts`      | M3+M4B      | a fixed `LlmSentence` flows through `TtsBridge` → `OpenAiTtsClient` (Speaches) → `PlaybackEngine` |
| `chat`     | M1+M3+M4B   | full text-in → speech-out loop: fixed prompt → LLM → SentenceSplitter → TTS → speakers |
| `loopback` | M4          | mic → speakers passthrough through the SPSC ring + 48↔16 kHz resampler |
| `capture`  | M4          | mic capture + Silero VAD endpointing report (no STT) |
| `stt`      | M4B         | self-contained STT smoke: TTS-fixture audio → Speaches `/v1/audio/transcriptions` |

Each demo exits with `0` on success and non-zero with a clear failure
line on stderr. They use the same config-resolution path as `acva`
itself — pass `--config PATH` to override.

When something doesn't work, [`docs/troubleshooting.md`](../../docs/troubleshooting.md)
is the symptom-first guide: it routes "I hear nothing" / "no LLM
reply" / "backend down" / "audio choppy" to the right demo command
and explains how to read the output. Future milestones (M4–M8) plan
their own demo commands in their respective milestone files; once
they land, the troubleshooting guide picks them up via the
component-cross-reference table at the bottom.

## What you can try after each milestone

A short tour of the user-touchable surface each milestone delivers — the
shortest commands that exercise its headline feature. All commands assume
the project root as cwd and a successful `./build.sh` (= dev preset).

### M0 ✅ — skeleton runs end-to-end on synthetic events

The orchestrator boots, the FSM cycles, and the control plane answers.
No LLM, no audio, no SQLite — but the scaffolding is real. The fake
driver ships off by default; flip it on for an M0-style demo:

```sh
# Enable the synthetic FSM driver via config (default is off):
sed -i 's/fake_driver_enabled: false/fake_driver_enabled: true/' \
    config/default.yaml
./_build/dev/acva
# in another terminal:
curl -s http://127.0.0.1:9876/health              # → "ok"
curl -s http://127.0.0.1:9876/status | jq         # FSM state + turn counters tick
curl -s http://127.0.0.1:9876/metrics | grep voice_turns_total
```

Watch the JSON-per-line log on stderr stream `fsm <prev> -> <next>`
transitions; `voice_turns_total{outcome="completed"}` increments roughly
once per `fake_idle_between_turns_ms`.

One-shot version: `./_build/dev/acva demo fsm` runs the synthetic
driver and exits.

### M1 ✅ — talk to a real LLM, memory persists across restarts

Bring up the compose stack (llama+whisper+piper containers), then drive
the orchestrator from stdin and watch streamed sentences come back.

```sh
cd packaging/compose && docker compose up -d      # one-time pull, then warm starts
cd ../..
./_build/dev/acva --stdin --config config/default.yaml
> what's the weather like on the moon?            # watch sentences stream back
> ^D                                              # clean exit; session closes
sqlite3 acva.db "select id, role, lang, status, substr(text,1,40) from turns;"
./_build/dev/acva --stdin --config config/default.yaml   # second run inherits memory
```

Kill the binary mid-turn (`kill -9`) and restart: the startup recovery
sweep flips the dangling `in_progress` turn to `interrupted`. The
`voice_llm_first_token_ms` histogram populates on the first reply.

Want a no-input smoke check? `./_build/dev/acva demo llm` sends a
fixed prompt straight to `LlmClient` and prints the streamed reply.

### M2 ✅ — supervision + dialogue gating + keep-alive

The supervisor probes each backend's `/health`, gates the dialogue path
when a critical backend goes unhealthy, and pings the LLM during idle to
keep it warm.

```sh
./_build/dev/acva --stdin --config config/default.yaml &   # backends already up
curl -s http://127.0.0.1:9876/status | jq '.pipeline_state, .services'
curl -s http://127.0.0.1:9876/metrics | grep -E 'voice_(health_state|pipeline_state|llm_keepalive)'

# Force-fail the LLM and watch gating kick in:
docker compose -f packaging/compose/docker-compose.yml stop llama
# Within ~5 s: services[].state flips to "degraded", then "unhealthy"
# After supervisor.pipeline_fail_grace_seconds: pipeline_state → "failed".
# Type a stdin line — refused, one log line per minute.

docker compose -f packaging/compose/docker-compose.yml start llama
# Recovers within a couple of probe intervals; pipeline_state → "ok".
```

Idle the dialogue and watch `voice_llm_keepalive_total{outcome="fired"}`
tick once every `cfg.llm.keep_alive_interval_seconds`.

`./_build/dev/acva demo health` is the one-shot equivalent: probes
every configured backend once and prints `ok / http / latency` per
service.

### M3 ✅ — Piper TTS + playback queue

Typed-in or LLM-generated sentences are spoken through the speakers via
Piper, with per-language voice routing and a sequence-tagged playback
queue.

```sh
./_build/dev/acva --stdin --config config/default.yaml
> hi, how are you?              # plays back through the default audio device
> ¿qué tal?                     # routes to es voice if cfg.tts.voices[es] is set
curl -s http://127.0.0.1:9876/metrics | grep voice_tts_first_audio_ms
```

Two no-input demos cover the M3 surface:

```sh
./_build/dev/acva demo tone     # 1.5 s 440 Hz sine — verifies sound output, no Piper
./_build/dev/acva demo tts      # synthesizes "Hello from acva." through Piper
```

`tone` is the one to run first when troubleshooting "I hear nothing":
it isolates the audio device + PortAudio path from the Piper config.

### M4 ✅ — mic capture + VAD endpointing

Speak into the mic; the orchestrator emits `SpeechStarted` /
`SpeechEnded`, captures the utterance, and shows it via `/status`. STT
isn't wired yet — the fake driver still supplies the transcript.

```sh
./_build/dev/acva --config config/default.yaml      # fake_driver gives transcript
# Speak. JSON log lines `event:"speech_started"` / `"speech_ended"` appear.
curl -s http://127.0.0.1:9876/metrics | grep -E 'voice_vad_(false_starts|onsets)_total'
```

Two no-input demos cover the M4 surface:

```sh
./_build/dev/acva demo loopback   # mic → speakers passthrough; verifies SPSC + resampler
./_build/dev/acva demo capture    # mic + VAD endpointing report; tunes thresholds
```

### M4B ✅ — voice-backend consolidation onto Speaches

One Speaches container replaces the separate `whisper.cpp/server` and
`piper.http_server` Compose services. Same audio output to a listener,
same `acva demo tts` / `acva demo chat` behaviour — but now over an
OpenAI-compatible HTTP surface, and the synthetic-`FinalTranscript`
hole in M4 is closed: real STT lands on the bus.

```sh
# Compose stack is now `llama` + `speaches`:
docker compose -f packaging/compose/docker-compose.yml up -d
./scripts/download-speaches-models.sh        # idempotent
curl -fsS http://127.0.0.1:8090/health        # 200

./_build/dev/acva --config config/default.yaml --stdin
> what's the moon doing?                      # → LLM → Speaches TTS → speakers

./_build/dev/acva demo stt                    # self-contained STT smoke
./_build/dev/acva demo tts                    # streaming TTS via Speaches
```

Streaming TTS (the new TTFB win) currently produces playback
underruns until the M4B follow-up `PlaybackEngine` pre-buffer threshold
lands — see memory note `project_m4b_tts_underruns.md`.

### M5 (planned) — streaming STT + speculation

End-to-end voice conversation in any language Whisper handles: speak a
question, hear a spoken answer. Speculation may start the LLM on a
stable partial before the final transcript lands.

```sh
./_build/dev/acva --config config/default.yaml
# Speak in English, then Russian. Replies come back in the spoken language.
curl -s http://127.0.0.1:9876/metrics | grep -E 'voice_(stt_partials|speculation_(kept|restarted))_total'
```

### M6 (planned) — AEC clears speaker echo

The assistant's own voice through speakers no longer triggers VAD. You
can hold a hands-free conversation without headphones.

```sh
./_build/dev/acva --config config/default.yaml
# Use built-in speakers (no headphones). Long replies don't self-trigger.
curl -s http://127.0.0.1:9876/metrics | grep -E 'voice_aec_(delay_estimate_ms|erle_db)'
```

### M7 (planned) — barge-in

Interrupt the assistant mid-sentence by speaking. Playback drains within
a few hundred milliseconds; the assistant turn is persisted as
`Interrupted` (or `Discarded` if no sentence finished).

```sh
./_build/dev/acva --config config/default.yaml
# Ask a long question; speak again while the answer plays.
curl -s http://127.0.0.1:9876/metrics | grep voice_barge_in_latency_ms
sqlite3 acva.db "select id, status, interrupted_at_sentence from turns order by id desc limit 5;"
```

### M8 (planned) — production hardening

Long-running soak harness, hot-reload, privacy commands, and a packaged
deployment.

```sh
scripts/soak.sh 4h                                # 4-hour scripted exchange
curl -X POST http://127.0.0.1:9876/reload         # safe field hot-reload
curl -X POST http://127.0.0.1:9876/mute           # toggle VAD intake
curl -X POST 'http://127.0.0.1:9876/wipe?session=42'
systemctl --user start acva.target                # systemd-packaged dev path
```

## Conventions used in these documents

- **Step**: an ordered chunk of work suitable for a single PR (or, in solo dev, a single coherent push).
- **Files**: paths under `src/` and `tests/` that the milestone creates or substantially modifies.
- **Acceptance**: the observable behavior that must hold before the milestone closes. Anything not listed is not in scope for this milestone.
- **Risks**: things that have a real chance of derailing the milestone, with the mitigation we've already chosen.

When a milestone surfaces a new design question, log it in `../open_questions.md` rather than mutating the milestone plan in place.
