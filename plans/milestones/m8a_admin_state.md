# M8A — Admin & State Management

**Estimate:** ~2 weeks.

**Depends on:** M0–M7. Sibling sub-milestones M8B (observability + soak) and M8C (distribution + wake-word) can run in parallel after M8A's hot-reload + privacy surface is in place.

**Blocks:** MVP release (with M8B + M8C).

## Goal

The admin / control-plane half of M8: everything an operator uses to inspect, mutate, and recover the running orchestrator's state without touching the binary or the SQLite file by hand. Five surfaces:

1. Hot-reload — change config without restart.
2. Privacy commands — mute / new session / wipe at runtime.
3. Memory CRUD CLI — offline DB access for forensics & cleanup.
4. Watchdog + checkpointed restart — detect stuck, exec a fresh process, resume the live conversation.
5. Boot-time model orchestration — single source of truth for the loaded LLM model, with optional strict-startup gating.

The split from the original M8 is purely organizational: M8 was growing past 11 steps. This sub-milestone groups the admin / state-management work so each sub-milestone is ~1 week and ships independently.

## Out of scope

- New user-facing features (covered by M8C: wake-word).
- Soak harness, dashboard, OTLP (covered by M8B).
- Packaging, docs, final sweep (covered by M8C).

## Step 1 — Config hot-reload

Per the locked design:
- Hot-reloadable: `llm.{temperature, max_tokens}`, `dialogue.{max_assistant_sentences, max_tts_queue_sentences}`, `vad.{onset_threshold, offset_threshold, hangover_ms}`, `tts.speed`, `logging.level`.
- Restart-required: audio device, sample rate, model paths, service endpoints, pipeline graph topology, DB path.

Trigger: `POST /reload` endpoint or `SIGHUP`.

**Behavior:**
- Re-read config file.
- Diff against current Config.
- For each changed field:
  - If hot-reloadable: apply (re-set logger level, push new VAD thresholds to endpointer, etc.).
  - If restart-required: reject the entire reload, return HTTP 409 with the offending fields, do not apply anything.
- Log `config: reloaded N hot-fields, ignored 0 restart-required fields`.

**Files:**
- `src/config/reload.hpp`, `src/config/reload.cpp`.
- Add `Reloadable` interface implemented by components with hot-reloadable settings.

## Step 2 — Privacy commands

Wire the remaining HTTP endpoints:

- `POST /mute` / `POST /unmute` — toggles VAD intake. The endpointer ignores frames while muted.
- `POST /new-session` — the SessionManager closes the current session (sets `ended_at`), opens a new one. Subsequent turns belong to the new session.
- `POST /wipe?session=<id>` — deletes the named session and its cascade (turns, summaries).
- `POST /wipe?all=true` — drops and recreates the schema; deletes audio files if recording was on.

`SessionManager` is a new small component:
- `src/dialogue/session.hpp/cpp`.
- Owns the current session id; transitions on idle timeout (default 30 min) or explicit command.

## Step 3 — Memory CRUD CLI

A small offline command-line tool that opens the SQLite DB directly
(no live orchestrator required) and exposes list / show / delete /
wipe operations against sessions, turns, facts, and summaries.
Complements Step 2: privacy commands act on the running orchestrator
via HTTP; this tool acts on the cold DB. Useful for forensics,
manual cleanup, and recovering from corrupted state without booting
the full stack.

**Surface** — invoked as a subcommand of the existing `acva` binary so
no new build target is needed:

```text
acva memory <command> [options]

  sessions   [--limit N] [--json]            list sessions, newest first
  session    <id> [--turns]                  show one session (+ its turns)
  turns      [--session ID] [--limit N]      list turns, newest first
  turn       <id>                            show one turn (full text, lang, status)
  facts      [--session ID]                  list facts (M1 schema)
  summaries  [--session ID]                  list summaries
  delete-turn <id>
  delete-session <id>                        cascade-deletes turns + summaries
  delete-fact <id>
  wipe       [--yes]                         drops and recreates the schema
  vacuum                                     SQLite VACUUM to reclaim space
```

All commands honor `--config PATH` and `--db PATH` overrides; default
resolves the DB the same way `acva` itself does (`cfg.memory.db_path`
→ XDG fallback). `--json` switches output from human-readable tables
to one JSON object per line for piping into `jq`.

**Files:**
- `src/cli/memory_cli.{hpp,cpp}` — dispatch on `argv[1] == "memory"`
  in `main.cpp`, wired before the demo dispatcher.
- Reuses `src/memory/repository.cpp` verbatim — no new SQL, just a
  formatter layer over existing methods. Read paths use
  `MemoryThread::read`; write/delete paths use `MemoryThread::post`
  with a 5 s deadline so the CLI exits promptly on errors.

**Safety rails:**
- `wipe` requires `--yes` on the command line (no prompt — the CLI
  is meant to be scriptable). Logs the row counts it's about to drop
  before doing it.
- `delete-session`/`delete-turn` print the target row + a one-line
  "1 turn / 4 sentences will be removed" preview, then act unless
  `--dry-run`.
- The orchestrator must NOT be running while `wipe` / `delete-*` are
  invoked — SQLite WAL mode tolerates concurrent readers but not
  concurrent writers across processes. The CLI checks for a stale
  lockfile and refuses if one is present, with a clear message
  pointing at `pkill acva` or `systemctl --user stop acva`.

**Tests:**
- Unit: `tests/test_memory_cli.cpp` — drives each subcommand against
  a temp SQLite DB, asserts on output + post-state.
- Integration: drive `acva memory` against a real session created
  by `acva --stdin` (in `run_integration_tests.sh`).

**Why this lives in M8 and not earlier:** the schema's been stable
since M1 but has accumulated facts (M5 stretch) + summaries (M1 part
A) + interruption metadata (M7). Building the CLI before that surface
settled would have meant rewriting it on every milestone.

## Step 4 — Watchdog + checkpointed restart

A pair of mechanisms that close the loop on "the orchestrator is
stuck and I need to bounce it without losing the conversation":

- **Watchdog** detects prolonged inactivity and either logs it (default)
  or triggers an automatic checkpointed restart (`cfg.supervisor.auto_restart_on_stuck: true`).
- **`POST /restart`** (and `acva memory restart` from Step 3) flushes
  a runtime checkpoint to SQLite and `exec`s a fresh `acva` process.
- **Resume on startup** reads the checkpoint row inside the existing
  M1 recovery sweep; if the timestamp is recent, the orchestrator
  picks up where it left off instead of opening a new session.

This is admin-driven control alongside Step 2 (privacy) and Step 3
(memory CLI), and it complements M1's crash-recovery (which marks
in-progress turns interrupted on cold restart): warm restart
preserves the active turn id, FSM state, and last partial so the
user doesn't see "session reset" mid-conversation.

### Watchdog (passive)

A thin observer over the bus + supervisor health, in `src/supervisor/watchdog.{hpp,cpp}`:

- Liveness signals tracked:
  - last `LlmToken` while FSM is in `Thinking`
  - last `TtsAudioChunk` while FSM is in `Speaking`
  - last `SpeechStarted` / `FinalTranscript` cadence over the last N minutes
- Threshold: `cfg.supervisor.stuck_threshold_seconds` (default 90 s).
  Configurable per-state — `Thinking` state warrants a tighter window
  than `Listening` does.
- On firing: emits a structured log line + `voice_stuck_total` metric.
  No restart unless `cfg.supervisor.auto_restart_on_stuck: true`.

### Checkpoint table (SQLite)

One singleton row in a new `runtime_state` table:

```sql
CREATE TABLE IF NOT EXISTS runtime_state (
  id              INTEGER PRIMARY KEY CHECK (id = 1),  -- enforce singleton
  session_id      INTEGER NOT NULL REFERENCES sessions(id),
  active_turn_id  INTEGER,
  fsm_state       TEXT NOT NULL,
  last_partial    TEXT,
  config_hash     TEXT,
  checkpoint_at   INTEGER NOT NULL                    -- epoch ms
);
```

`ON CONFLICT (id) DO UPDATE` so writes are idempotent overwrites.

**One wrinkle:** writes through `MemoryThread::post` are async / fire-
and-forget. The pre-`exec` checkpoint must synchronously flush
before the process is replaced — otherwise we restart on an
unflushed write. New `Repository::checkpoint_runtime_sync(...)`
opens its own short-lived connection on the calling thread and
commits before returning, the same pattern M1's recovery sweep
already uses on startup.

### `POST /restart` + CLI

- HTTP endpoint: `POST /restart` triggers a graceful `Manager::stop()` →
  `Repository::checkpoint_runtime_sync(...)` → `execv(argv[0], argv)`.
  Returns 202 on accept; the connection drops as the process exec's.
- CLI: `acva memory restart` (in Step 3's CLI) does the same against
  the running orchestrator's HTTP control plane — one fewer thing to
  remember.
- Refuses to restart if a previous checkpoint is younger than 5 s
  (debounce: avoids restart loops if the watchdog mis-fires).

### Resume on startup

In `src/memory/recovery.cpp` (already opens the DB on startup):

- Read the singleton row.
- If `now() - checkpoint_at <= cfg.supervisor.checkpoint_max_age_seconds`
  (default 60 s) AND `config_hash` matches the freshly-loaded config:
  reuse `session_id` + `active_turn_id`; restore the FSM via
  `Fsm::resume_at(state)`; replay `last_partial` to the bus so any
  subscriber that cares (Manager waiting on a transcript) sees it.
- Otherwise: clear the checkpoint row and fall through to the normal
  M1 path (mark in-progress turns interrupted, open new session).

`config_hash` mismatch is the safety guard: if the user restarted
to apply config changes that change the FSM topology or the model
identity, we don't paper over them by silently resuming.

### Tests

- Unit: `tests/test_watchdog.cpp` — drive the bus with paused activity,
  assert the watchdog fires + does/doesn't trigger restart per config.
- Unit: `tests/test_recovery.cpp` extends — checkpoint row → resume
  populates FSM correctly; stale row → discard and normal recovery.
- Integration: live `acva` → `POST /restart` → process restarts within
  2 s → `/status` shows the same turn id continued.

## Step 5 — Boot-time model orchestration

Two related capabilities that put acva in charge of "what models are
loaded and ready when I'm running" without breaking pillar #1
(orchestrator never forks a backend).

Today the asymmetry is real: `cfg.llm.model` is the API alias acva
sends in `model:` fields, but the loaded GGUF is chosen by
`ACVA_LLM_MODEL` on the compose CLI. Two sources of truth that can
drift silently. acva also accepts "degraded mode" boots where one or
more backends are unhealthy — useful during M0–M5 iteration but a
footgun in production.

### Step 5.a — `model-controller` sidecar

A small **separate Go binary** that runs as its own Compose service
(`packaging/compose/docker-compose.yml` adds `model-controller`).
Listens on `127.0.0.1:9877` and exposes a tiny REST surface:

```text
POST /llm/load
    body: {"file": "Outlier-Lite-7B-Q4_K_M.gguf"}
    202 — recreate started; poll /llm/status
GET  /llm/status
    200 {"loaded_file": "...", "alias": "...", "health": "healthy"}
```

Implementation: the controller has the docker socket bind-mounted
read-write and runs `docker compose --project-directory /compose
up -d --force-recreate llama` after writing
`packaging/compose/.env` with `ACVA_LLM_MODEL=<file>`. It blocks
until llama's `/health` returns 200 with the requested model
visible at `/v1/models`, then returns 200 to the client.

**Why a sidecar, not part of acva itself:**
- Pillar #1 stands: acva still doesn't have docker socket access
  and never invokes `compose` commands. The controller takes the
  privilege; acva is its HTTP client.
- The controller is also useful from a CLI / shell — `curl -d ...`
  swaps models without restarting acva.
- Why Go: trivial static binary; one file under
  `packaging/model-controller/main.go`; compiles to ~6 MB; doesn't
  pull a Python runtime into Compose.
- **Why NOT a proxy:** requests still flow acva → llama directly,
  no extra hop on the LLM hot path. The controller is invoked once
  at startup (and on rare config changes), not per request — so
  cold-load latency hits boot time, not per-turn latency.

acva-side wiring lives in `src/llm/model_controller_client.{hpp,cpp}`:
- On `acva` startup, after config load, call
  `POST /llm/load {"file": cfg.llm.model_file}` if the running
  llama is serving a different file (read via `/v1/models` on the
  llama service before deciding).
- Returns once the controller reports the swap done; falls through
  to the existing connect-and-go path.

New config field: `cfg.llm.model_file` (filename only, no path).
Replaces the silent dependency on `ACVA_LLM_MODEL`. The downloader
(`scripts/download-llm.sh`) gives users the canonical filenames.

### Step 5.b — Strict startup: force-load + self-check

A new boot-time gate that, when enabled, refuses to start if any
required component fails its smoke. Off by default to preserve M0–M7
"degraded mode is fine" UX; opt-in via:

```yaml
supervisor:
  strict_startup: true   # default false
  startup_force_load: true   # default true when strict_startup is true
```

**What runs at startup, in order:**

1. **Per-service `/health` probes** — already happen via the
   supervisor's first probe pass. `strict_startup` upgrades any
   `fail_pipeline_if_down: true` service from "log warning + gate
   dialogue" to "exit non-zero with structured error".
2. **Force-load** (if `startup_force_load`):
   - llama: `POST /v1/chat/completions` with a 1-token request.
     This blocks until weights + KV cache are warm in VRAM (~3-5 s
     cold). Catches "container up but model file missing/corrupt".
   - Speaches STT: `POST /v1/audio/transcriptions` with a 0.5 s
     silent WAV fixture. Loads the configured `cfg.stt.model` into
     VRAM.
   - Speaches TTS: `POST /v1/audio/speech` with a 5-character input
     against the configured English voice. Loads Piper into VRAM.
   - Each round-trip has a 30 s deadline; failure logs the failing
     component + the underlying error and (under strict_startup)
     exits non-zero.
3. **Capture readiness probe** (when `cfg.audio.capture_enabled`):
   open the input device for a 100 ms read; if PortAudio falls
   back to headless, that's a strict-mode failure (the M5 default
   pipeline expects a real mic).
4. **Memory recovery sweep** — already happens; strict mode treats
   "DB unreadable" as fatal instead of "log + continue with a fresh
   schema".

**The "log errors if anything required fails, not starting in
degraded mode" UX:**

- Default mode (`strict_startup: false`): each failure logs at
  WARN with a `start_check_failed` event; orchestrator continues
  and the supervisor's existing `pipeline_state == failed` gating
  kicks in for the affected path. Same as today.
- Strict mode (`strict_startup: true`): each failure logs at ERROR
  with the same event and a remediation hint ("LLM model file
  missing — run `scripts/download-llm.sh <alias>`"). After all
  checks run (so the operator sees every failure, not just the
  first), acva exits 1.

**Files:**
- `src/supervisor/startup_check.hpp/cpp` — orchestrates the gates.
- `packaging/model-controller/main.go` (+ Dockerfile, compose
  service entry) — the sidecar.
- `src/llm/model_controller_client.hpp/cpp` — acva's HTTP client.
- `tests/test_startup_check.cpp` — gated unit tests using fake
  servers.

### Tests

- `model-controller` unit: spawn a fake llama, drive `/llm/load`,
  assert the env file is rewritten + the recreate was issued.
- Strict-mode unit: each failure mode exits non-zero with the
  expected event in the captured log.
- Integration (gated): `acva` boots green against a fully-warm
  Compose stack. Then kill `llama`; with `strict_startup: true`
  acva exits 1; with `false` acva continues and the supervisor
  reports the path gated.

## Demo commands (planned)

### `acva demo wipe` — privacy-command roundtrip

Opens a tmp DB, writes a session with a few turns, calls `POST
/wipe?session=<id>` then `POST /wipe?all=true`, and verifies row
counts went to zero on each step.

```
demo[wipe] tmp db=/tmp/acva-demo-wipe-NNN.db
  step 1: insert 1 session + 3 turns → rows=4
  step 2: POST /wipe?session=N → rows=1 (just the wiped session marker)
  step 3: POST /wipe?all=true → rows=0
demo[wipe] PASS
```

### `acva demo reload` — hot-reload smoke

Boots minimally, mutates a hot-reloadable field on disk
(`logging.level: info` → `debug`), calls `POST /reload`, then logs at
debug level to confirm it took effect. Also tries a restart-required
field (`memory.db_path`) and asserts a 4xx with a clear message.

```
demo[reload] hot-reload field 'logging.level': info → debug
  POST /reload → 200 (took 12ms)
  test log line at debug level: visible ✓
demo[reload] restart-required field 'memory.db_path' rejected with 400 — message clear ✓
demo[reload] PASS
```

## Acceptance

1. **Hot-reload works.** Changing log level via `POST /reload` takes effect within 1 second. Restart-required fields are rejected with HTTP 409.
2. **Wipe works.** `POST /wipe?all=true` empties the database and audio dir; new turns create a fresh session.
3. **Memory CRUD CLI works.** `acva memory sessions`, `acva memory delete-session <id>`, `acva memory wipe --yes` all behave on a real DB without the orchestrator running. CLI integration test in `run_integration_tests.sh` is green.
4. **Watchdog + restart works.** Stuck orchestrator → `voice_stuck_total` increments. `POST /restart` returns 202, the new process is up within 2 s, the active session id and turn id are preserved (warm restart). Cold restart (stale or absent checkpoint, or `config_hash` mismatch) falls back to the M1 recovery path with no regression.
5. **Boot-time model orchestration works.** Setting `cfg.llm.model_file` to a different downloaded GGUF and starting `acva` causes the controller sidecar to recreate llama with that file; acva connects with the matching alias and runs end-to-end. With `strict_startup: true` and a missing model, acva exits non-zero within 5 s with a single ERROR per failed component and a remediation hint.

## Risks specific to M8A

| Risk | Mitigation |
|---|---|
| Hot-reload corrupts running state | Apply changes only via component-owned methods; never reach into private state from outside |
| Watchdog mis-fires during legitimate quiet (long LLM completion, user pause) | Conservative defaults (90 s in `Thinking`); per-state thresholds; auto-restart off by default |
| Restart loop on a structurally stuck state | 5 s debounce in `POST /restart`; bounded attempts within a sliding window |
| Model controller widens attack surface (docker socket access) | Bind socket read-only where supported; controller listens on 127.0.0.1 only; controller has zero file-system writes outside `packaging/compose/.env` |
| Strict-startup boot regression — anyone enabling it sees nuisance failures | Off by default; opt-in via `cfg.supervisor.strict_startup`; M5 default config keeps the existing tolerant boot |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Hot-reload | 1.5 days |
| 2 Privacy commands | 1 day |
| 3 Memory CRUD CLI | 1.5 days |
| 4 Watchdog + checkpointed restart | 2 days |
| 5 Boot-time model orchestration | 3 days |
| **Total** | **~9 days = ~2 weeks** |
