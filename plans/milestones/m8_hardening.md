# M8 — Production Hardening

**Estimate:** 2 weeks.

**Depends on:** all of M0–M7.

**Blocks:** MVP release.

## Goal

Take the working pipeline and make it ship-ready: 4-hour soak passing, observability stack documented, hot-reload working, packaging in place, edge cases handled. After M8 the project is at MVP per `project_design.md` §19.

## Out of scope

- New features. M8 polishes existing ones.
- Tool calling (deferred entirely; F2).
- GUI (deferred to post-MVP).

## Step 1 — Soak test infrastructure

**Files:**
- `scripts/soak.sh` — runs the orchestrator + scripted user input for `$DURATION` (default 4 h).
- `scripts/soak-driver.py` — synthesizes user audio at random intervals, types prompts, logs assistant timing.
- `tests/soak/fixtures/` — recorded user audio clips for replay.

The soak driver:
- Picks a random prompt from a fixed corpus (~50 prompts).
- Plays the user audio through a virtual mic (PulseAudio loopback module).
- Waits for assistant playback to complete.
- Logs turn metrics (latency, outcome) to a CSV.
- Occasionally (15% of turns) injects mid-turn barge-in.
- Repeats for `$DURATION`.

After the run: a small post-processor reads the CSV + Prometheus metrics dump and produces a soak report:
- P50/P95 per stage (vad → final, final → first_token, first_token → first_audio).
- Heap RSS over time.
- Queue depths (max, mean).
- Service restart count.
- Turn outcome counts.

**Acceptance criteria** (re-stated from §14):
```
duration:        4 h
crashes:         0
heap growth:     < 50 MB after 1 h warmup
queue depth:     stable, no monotonic growth
latency P95:     within +20% of post-warmup baseline
service restarts: ≤ 2 (incidental), pipeline never enters failed state
```

## Step 2 — Metrics dashboard

Two options:
- **A.** Grafana dashboard JSON, points at a local Prometheus instance.
- **B.** Static HTML + Chart.js page served at `/dashboard`.

Recommend A for proper operability — standard tooling, can plug into other workstation Prometheus setups. Ship the dashboard JSON in `packaging/grafana/acva.json`. Document the Prometheus config snippet to scrape `acva.service` in the README.

Dashboard sections:
- **Latency** — VAD → final, final → first_token, first_token → first_audio, end-to-end.
- **Throughput** — turns/min, sentences/min.
- **Health** — service states, restart counts.
- **Audio** — playback underruns, AEC delay, queue depths.
- **Memory** — turns persisted, summary count, DB size on disk.

## Step 3 — Config hot-reload

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

## Step 4 — Privacy commands

Wire the remaining HTTP endpoints:

- `POST /mute` / `POST /unmute` — toggles VAD intake. The endpointer ignores frames while muted.
- `POST /new-session` — the SessionManager closes the current session (sets `ended_at`), opens a new one. Subsequent turns belong to the new session.
- `POST /wipe?session=<id>` — deletes the named session and its cascade (turns, summaries).
- `POST /wipe?all=true` — drops and recreates the schema; deletes audio files if recording was on.

`SessionManager` is a new small component:
- `src/dialogue/session.hpp/cpp`.
- Owns the current session id; transitions on idle timeout (default 30 min) or explicit command.

## Step 5 — OTLP wiring (opt-in)

Per H2: OTLP traces via `opentelemetry-cpp`. Disabled by default; enabled via `cfg.observability.otlp.endpoint`.

**Files:**
- `src/observability/otlp.hpp`, `src/observability/otlp.cpp`.

Each user turn is a span tree:
- Root span: `voice.turn` with `turn_id` attribute.
- Child spans: `vad`, `stt.partial`, `stt.final`, `prompt.assemble`, `llm.first_token`, `llm.stream`, `tts`, `playback`.

Span starts / ends are wired via callbacks the components already have (the observability hooks were left empty in M0–M7 for exactly this purpose).

OTLP export uses HTTP (not gRPC) to keep the dependency surface lean. Endpoint defaults to `http://127.0.0.1:4318/v1/traces` if a local otelcol-contrib is configured.

## Step 6 — Packaging

Two deployment paths ship side-by-side; both have been informally validated since M1.

### Dev path: Docker Compose (default since M1.B)

- `packaging/compose/docker-compose.yml` — already in tree from M1.B; finalized here:
  - LLM, STT (whichever M5 picked: whisper / speaches / faster-whisper), and per-language Piper services pinned to image digests (not `:latest` floating tags).
  - `.env.example` documented.
- `scripts/dev-up.sh` — `cd packaging/compose && docker compose up -d`, plus a model-availability check.
- `scripts/dev-down.sh` — symmetric.
- The orchestrator continues to run as a host CLI (`./_build/release/acva --config ...`).

### Production path: systemd units (alternative)

- `packaging/systemd/acva.service`, `acva-llama.service`, `acva-whisper.service`, `acva-piper.service`, `acva.target` — finalized; were placeholders since M2.
- `scripts/install-systemd.sh` — copies units to `~/.config/systemd/user/`, runs `systemctl --user daemon-reload`. The script defers binary install to the user (or a downstream package).
- `scripts/uninstall-systemd.sh` — symmetric.
- Switching to this path requires `cfg.supervisor.bus_kind: user` and recompilation with `-DACVA_ENABLE_SDBUS=ON` (gates the optional sd-bus client described in m2_supervision.md's "systemd alternative" section).
- `packaging/man/acva.1` — man page (terse), independent of deployment path.

### Optional (stretch)

- AUR `PKGBUILD` for Arch / Manjaro.
- `.deb` build script for Debian/Ubuntu.

## Step 7 — Documentation pass

- `README.md` — installation steps refined based on real-user experience during M0–M7. (Already partially done; needs final pass.)
- `docs/operations.md` — runbook for "the LLM is unhappy", "the mic isn't picked up", common failure modes.
- `docs/configuration.md` — full reference of every config field, with default and notes.
- `docs/architecture.md` — distilled summary for new contributors.

## Step 8 — Final sweep

- Address every TODO in the codebase (or open an issue for it).
- Run clang-tidy with the project's `.clang-tidy` config; fix or suppress.
- Make sure every public function has at least a one-line comment when the *why* is non-obvious (per CLAUDE.md guidance).
- Run the full test suite under ASan, UBSan, TSan once each.

## Test plan

The big one is the soak test (Step 1). On top of that:
- Reload tests: hot-reload changes log level mid-run; observable in logs immediately.
- Restart-required reload rejected with the right HTTP status.
- Wipe tests: data gone; new turns work after.
- OTLP smoke (gated): with otelcol-contrib running, one full turn, one root span with the expected children.

## Acceptance

1. **4-hour soak passes.** All criteria met. Report committed to `tests/soak/reports/` with date and git revision.
2. **Hot-reload works.** Changing log level via `POST /reload` takes effect within 1 second.
3. **Wipe works.** `POST /wipe?all=true` empties the database and audio dir; new turns create a fresh session.
4. **OTLP traces visible** in a local otelcol when enabled; no impact when disabled.
5. **Both deployment paths work** end-to-end on a clean Manjaro and a clean Ubuntu 24.04 VM:
   - Docker Compose: `docker compose up -d && ./scripts/dev-up.sh` brings up backends; `./_build/release/acva` connects on the host.
   - systemd: `./scripts/install-systemd.sh && systemctl --user start acva.target` brings up the full stack as units; `systemctl --user status` shows all four `active (running)`.
6. **Documentation complete.** A new contributor can read README + architecture.md and understand the system.

## Risks specific to M8

| Risk | Mitigation |
|---|---|
| Soak finds a leak that wasn't in earlier dev | Buffer 0.5 weeks for fixes; profile with heaptrack |
| Hot-reload corrupts running state | Apply changes only via component-owned methods; never reach into private state from outside |
| OTLP export contention with critical path | Async + non-blocking; if exporter blocks, drop spans |
| Packaging breaks on non-default file layouts | XDG-compliant paths; document overrides |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Soak infra + first run | 3 days |
| 2 Dashboard | 1 day |
| 3 Hot-reload | 1.5 days |
| 4 Privacy commands | 1 day |
| 5 OTLP | 1.5 days |
| 6 Packaging | 1.5 days |
| 7 Docs | 1.5 days |
| 8 Final sweep | 1 day |
| **Total** | **~12 days = ~2.5 weeks** |
