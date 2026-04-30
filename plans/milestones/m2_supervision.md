# M2 — Service Supervision (HTTP probes)

**Estimate:** ~3 days.

**Depends on:** M1 (LLM client to probe).

**Blocks:** M3 (TTS supervision shares the same code path), M5 (STT same).

## Goal

Application-level health monitoring for the three backend services. The orchestrator probes `/health` on each, transitions a per-service state machine, publishes `HealthChanged` events, and gates the dialogue path when a critical backend is unhealthy.

**The orchestrator does not run or restart processes.** Docker Compose's `restart: unless-stopped` (M1.B) does that. Our supervisor runs *on top of* Compose: it knows about *application* state, not process state.

This is a significant simplification from the earlier plan (sd-bus + systemd-unit state machine + restart issuing). The earlier plan still applies if you want a systemd-based production deployment — see §"systemd alternative" below.

## Out of scope

- Process lifecycle management (delegated to Compose / systemd).
- Restart command issuance.
- sd-bus client (now optional; described in §"systemd alternative").

## New deps

None. cpp-httplib (vendored in M0) is sufficient for non-streaming HTTP probes; libcurl (added in M1) is unused by M2 itself.

## Step 1 — Health probe

**Files:**
- `src/supervisor/probe.hpp`
- `src/supervisor/probe.cpp`

A simple synchronous HTTP GET wrapper. Returns `{ok, latency_ms, http_status, body_excerpt}`. Per-probe timeout from config (default 3 s). Non-2xx is non-fatal — that's data for the state machine.

```cpp
struct ProbeResult {
    bool ok = false;
    int http_status = 0;
    std::chrono::milliseconds latency{0};
    std::string body_excerpt;          // first 200 chars on failure
};

class HttpProbe {
public:
    explicit HttpProbe(std::chrono::milliseconds timeout);
    ProbeResult get(std::string_view url);
};
```

## Step 2 — Service state machine

**Files:**
- `src/supervisor/service.hpp`
- `src/supervisor/service.cpp`

```
NotConfigured → Probing → Healthy
                    ↓
               Degraded ↔ Healthy   (transient probe failure within tolerance)
                    ↓
               Unhealthy             (consecutive failures > threshold)
```

There is no `Restarting` or `Disabled` state — the orchestrator doesn't restart services itself. Compose handles it. If the backend stays unhealthy long enough, Compose's restart policy will eventually bring it back; we just observe and report.

Each transition publishes a `HealthChanged` event on the bus.

```cpp
struct ServiceConfig {
    std::string name;          // logical: "llm", "stt", "tts"
    std::string health_url;    // http://127.0.0.1:8081/health
    bool fail_pipeline_if_down = true;
    std::chrono::milliseconds probe_interval_healthy{5000};
    std::chrono::milliseconds probe_interval_degraded{1000};
    int degraded_max_failures = 3;
};

class ServiceMonitor {
public:
    ServiceMonitor(ServiceConfig cfg, HttpProbe& probe, event::EventBus& bus,
                   metrics::Registry& metrics);
    void start();   // launches its own probe thread
    void stop();
    [[nodiscard]] event::HealthState state() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point last_ok_at() const noexcept;
};
```

## Step 3 — Supervisor

**Files:**
- `src/supervisor/supervisor.hpp`
- `src/supervisor/supervisor.cpp`

Owns a `ServiceMonitor` per registered service. Aggregates state for `/status`. Decides pipeline-level fail conditions:

- If `cfg.supervisor.fail_pipeline_if_llm_down` and the LLM service stays `Unhealthy` for `pipeline_fail_grace_seconds`: emit `ErrorEvent` and the Dialogue Manager refuses new turns until recovery.

```cpp
class Supervisor {
public:
    Supervisor(const config::SupervisorConfig& cfg,
               event::EventBus& bus,
               metrics::Registry& metrics);
    ~Supervisor();

    void register_service(ServiceConfig cfg);
    void start();
    void stop();

    [[nodiscard]] std::vector<ServiceSnapshot> snapshot() const;
};
```

`ServiceSnapshot` exposed in `/status`:
```json
"services": [
  {"name":"llm","state":"healthy","last_ok_ms_ago":1421,"failures":0},
  {"name":"stt","state":"degraded","last_ok_ms_ago":4500,"failures":2},
  {"name":"tts","state":"healthy","last_ok_ms_ago":612,"failures":0}
]
```

## Step 4 — LLM keep-alive

While idle (FSM in `Listening`), submit a 1-token completion to the LLM every `cfg.llm.keep_alive_interval_seconds`. Skips during active turns. Hidden under a `voice_llm_keepalive_total` counter so it doesn't pollute the per-turn latency histograms.

**File:** `src/supervisor/keep_alive.hpp/cpp`. Owns its own timer thread; uses `LlmClient::keep_alive()` from M1.

## Step 5 — Pipeline gating

When the LLM is in `pipeline_failed` state (post-grace `Unhealthy`):

- Dialogue Manager (M1) refuses new `FinalTranscript`-driven turns. It transitions the FSM back to `Listening` and logs once per minute.
- `/status` includes `pipeline_state: "failed"`.
- When LLM recovers, normal flow resumes automatically.

## Step 6 — Config

```yaml
supervisor:
  pipeline_fail_grace_seconds: 30   # how long Unhealthy LLM has to recover before failing the pipeline
  probe_timeout_ms: 3000

# Per-service config attached under each backend's section:
llm:
  base_url: "http://127.0.0.1:8081/v1"
  health_url: "http://127.0.0.1:8081/health"   # explicit; could be derived
  fail_pipeline_if_down: true
  probe_interval_healthy_ms: 5000
  probe_interval_degraded_ms: 1000
  degraded_max_failures: 3
  keep_alive_interval_seconds: 60

stt:
  health_url: "http://127.0.0.1:8082/health"
  fail_pipeline_if_down: true   # consumed in M5
  # ... same probe knobs

tts:
  health_url: "http://127.0.0.1:8083/health"
  fail_pipeline_if_down: false   # M3 ships allow_tts_disabled=true
  # ... same probe knobs
```

## Test plan

| Test | Scope |
|---|---|
| `test_probe.cpp` | HTTP timeout / 2xx / 5xx / connection-refused all yield expected ProbeResult |
| `test_service_monitor.cpp` | mock probe; state-transition table including degraded↔healthy oscillation |
| `test_supervisor.cpp` | three services; fail_pipeline_if_down; pipeline gating; keep-alive timer |
| Manual integration | `docker compose stop llama`; supervisor flips to Unhealthy within probe_interval; pipeline gates after grace; `docker compose start llama`; recovery to Healthy; pipeline re-opens |

## Acceptance

1. With `docker compose up`, `/status` shows all three services `healthy`.
2. `docker compose stop llama`: supervisor reports `degraded` within `probe_interval_healthy`, then `unhealthy` after `degraded_max_failures` × `probe_interval_degraded`. `voice_health_state{service="llm",state="unhealthy"}` = 1.
3. After `pipeline_fail_grace_seconds` of LLM unhealthy: `/status.pipeline_state == "failed"`. Submitting a fake `FinalTranscript` does not start the LLM; the FSM logs once and stays Listening.
4. `docker compose start llama`: supervisor recovers to `healthy` within probe_interval_degraded × 2; `/status.pipeline_state == "ok"`.
5. Idle 90 seconds with the LLM up: `voice_llm_keepalive_total` increments by ~1 (depending on FSM state checks).

## Risks specific to M2

| Risk | Mitigation |
|---|---|
| Probe loop leaks under tight intervals | Single probe thread per service; bounded; tested under tsan |
| Health probe blocks pipeline thread | Probes run on the supervisor's own threads, not the dialogue thread |
| Compose's restart loop fights an unrecoverable failure (model file missing) | Supervisor reports unhealthy; user must intervene. Document in operations runbook (M8) |
| Keep-alive races with a real turn | Skip keep-alive unless FSM is `Listening` |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 HTTP probe | 0.5 day |
| 2 Service state machine | 0.5 day |
| 3 Supervisor + status | 0.5 day |
| 4 Keep-alive | 0.5 day |
| 5 Pipeline gating | 0.5 day |
| 6 Config + tests | 0.5 day |
| **Total** | **~3 days** |

---

## systemd alternative (production deployment)

Everything above describes the *dev* path running against Compose-managed containers. For a production deployment without Docker — e.g., a dedicated workstation running lclva long-term as `lclva.service` — the systemd path is still supported.

In that mode:

- Backends run as systemd units (see `packaging/systemd/`).
- The Supervisor *additionally* talks to `org.freedesktop.systemd1` over sd-bus to: read `ActiveState`/`SubState`, subscribe to `PropertiesChanged`, and (optionally) issue `RestartUnit` calls.
- Dependency: `libsystemd-dev`. CMake: `pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)`.
- Toggle: `cfg.supervisor.bus_kind: user | system | none`. `none` is the default and disables sd-bus entirely.

The sd-bus client and unit-state machine designed in the earlier draft of this milestone are still the right shape if/when this path lands. They're scoped to "post-M8 production hardening" — not on the critical path to MVP.

If you ever want this, files would be:
- `src/supervisor/sdbus_client.{hpp,cpp}` (RAII over `sd_bus*`, unit ops, signal subscription).
- Extension to `Supervisor` that subscribes for `PropertiesChanged` on each unit and incorporates systemd-reported state into the application state machine.

That's a ~1 week add-on, separate from MVP M2.
