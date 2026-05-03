# M8B — Observability & Soak

**Estimate:** ~1 week.

**Depends on:** M0–M7. Sibling sub-milestones M8A (admin & state) and M8C (distribution + wake-word). M8B can run in parallel with M8A since neither modifies the other's surface.

**Blocks:** MVP release (with M8A + M8C). The 4-hour soak passing is the headline acceptance gate that gives confidence to ship.

## Goal

The observability + correctness half of M8: verify the working pipeline holds up under sustained load, and document its internal behavior well enough that a maintainer can debug it from the outside. Three surfaces:

1. **Soak test infrastructure** — 4-hour scripted user/assistant exchange with leak/latency/restart criteria.
2. **Metrics dashboard** — Grafana JSON pinned to the Prometheus metrics already exposed since M2.
3. **OTLP wiring (opt-in)** — distributed traces for per-turn span trees.

The split from the original M8 is purely organizational: M8 was growing past 11 steps. This sub-milestone groups soak + observability work so it ships independently of admin features and packaging.

## Out of scope

- Admin / control-plane features (covered by M8A).
- Wake-word, packaging, docs, final sweep (covered by M8C).
- New features. M8 polishes existing ones.

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

## Step 3 — OTLP wiring (opt-in)

Per H2: OTLP traces via `opentelemetry-cpp`. Disabled by default; enabled via `cfg.observability.otlp.endpoint`.

**Files:**
- `src/observability/otlp.hpp`, `src/observability/otlp.cpp`.

Each user turn is a span tree:
- Root span: `voice.turn` with `turn_id` attribute.
- Child spans: `vad`, `stt.partial`, `stt.final`, `prompt.assemble`, `llm.first_token`, `llm.stream`, `tts`, `playback`.

Span starts / ends are wired via callbacks the components already have (the observability hooks were left empty in M0–M7 for exactly this purpose).

OTLP export uses HTTP (not gRPC) to keep the dependency surface lean. Endpoint defaults to `http://127.0.0.1:4318/v1/traces` if a local otelcol-contrib is configured.

## Demo commands (planned)

### `acva demo soak` — 60-second mini-soak

Runs the FakeDriver + (optional) real LLM/TTS for 60 seconds and
reports the same metrics the 4-hour harness does, but at a tractable
size. Useful as a pre-flight before kicking off the real soak.

Expected output:

```
demo[soak] duration=60s driver=fake llm=on tts=on
  t=10s  rss=215MiB  turns=4  underruns=0  queue_max=3   p95_first_audio=412ms
  t=30s  rss=218MiB  turns=12 underruns=0  queue_max=3   p95_first_audio=438ms
  t=60s  rss=220MiB  turns=24 underruns=1  queue_max=4   p95_first_audio=441ms
demo[soak] done: rss_growth=5MiB latency_p95_drift=+29ms supervisor_restarts=0
demo[soak] PASS (acceptance: rss_growth<50MiB, p95_drift<+20%, no supervisor restarts)
```

Failure modes:
- `rss_growth > 50 MiB / minute` → leak. The full 4-hour harness will catch it; this is a quick screen.
- `latency_p95_drift > 20%` → backpressure or thermal throttling.
- `underruns > 5 / minute` → playback queue starving; producer (LLM or Piper) is slow.

## Acceptance

1. **4-hour soak passes.** All criteria met. Report committed to `tests/soak/reports/` with date and git revision.
2. **Dashboard renders.** With Prometheus scraping acva and Grafana loading `packaging/grafana/acva.json`, every panel shows non-empty data after a 60 s mini-soak.
3. **OTLP traces visible** in a local otelcol when enabled; no impact when disabled.

## Risks specific to M8B

| Risk | Mitigation |
|---|---|
| Soak finds a leak that wasn't in earlier dev | Buffer 0.5 weeks for fixes; profile with heaptrack |
| OTLP export contention with critical path | Async + non-blocking; if exporter blocks, drop spans |
| Dashboard panels regress as metrics get renamed | Pin panel queries to a stable subset; add a CI step that diffs `/metrics` output against a golden list before merge |

## Step 4 — Build-time observability + reductions

A clean build (post-orchestrator-refactor, 107 TUs / 8 cores) takes
**~10 s wall** but **~556 s sequential**. One TU still dominates:
`src/config/config.cpp` at **~139 s** by itself — Glaze's YAML
reflection instantiates the full template tree for every nested
struct in the schema. The next tier (`supervisor/probe`,
`llm/client`, `stt/realtime_stt_client`, `http/server`, the
`test_manager` / `test_speaches_realtime_smoke` /
`test_tts_bridge` / `test_summarizer` units) all sit at 14–20 s
each because of heavy transitive includes (libdatachannel, libcurl,
cpp-httplib, doctest, glaze-driven config types).

Already landed (2026-05-03):
- `cmake -DACVA_TIME_TRACE=ON` adds `-ftime-trace`; per-TU JSONs
  drop next to each `.o` for chrome://tracing flamegraphs.
- `scripts/build-times.sh [preset] [top-N]` parses
  `_build/<preset>/.ninja_log` and prints a sorted wall-time table.
- `./build.sh` calls the script automatically; suppress with
  `ACVA_BUILD_TIMES=0`, change cap with `ACVA_BUILD_TIMES_TOP=N`.

To do under M8B:
1. **Split `src/config/config.cpp`** so each subsystem's parsing +
   validation lives in its own TU. Each per-section TU then
   instantiates only its own slice of the reflection tree, and the
   work parallelises across cores. Target: drop config.cpp's ~139 s
   to ~7 × ~20 s on 8 cores ≈ ~20 s wall.
2. **PCH for the test suites.** A 200-line `tests/test_pch.hpp`
   covering doctest + the most-shared `acva_core` public headers,
   wired via `target_precompile_headers`. Cuts the ~20 s tier in
   half — biggest impact on the 8 worst test TUs.
3. **pImpl for libcurl / libdatachannel / cpp-httplib consumers.**
   `llm/client.cpp`, `http/server.cpp`, `stt/realtime_stt_client.cpp`,
   `tts/openai_tts_client.cpp`, `stt/openai_stt_client.cpp` — wrap
   the third-party type in an `Impl` struct in the .cpp,
   forward-declare in the .hpp. Stops every downstream TU from
   re-parsing the library headers.

Acceptance for Step 4: clean `./build.sh` reports < 60 s wall on the
8-core dev workstation; no single TU > 60 s.

## Time breakdown

| Step | Estimate |
|---|---|
| 1 Soak infra + first run | 3 days |
| 2 Dashboard | 1 day |
| 3 OTLP | 1.5 days |
| 4 Build-time reductions (config split + PCH + pImpl) | 2 days |
| **Total** | **~7.5 days = ~1.5 weeks** |
