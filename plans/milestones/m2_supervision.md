# M2 — Service Supervision

**Estimate:** 1 week.

**Depends on:** M1 (LLM client to probe).

**Blocks:** M3 (Piper unit needs the same supervisor patterns), and any later milestone that wants robustness against backend death.

## Goal

The orchestrator manages the lifecycle of `lclva-llama.service` (and later `lclva-whisper.service`, `lclva-piper.service`) **as systemd units** via sd-bus — **no fork/exec of model runtimes from the orchestrator**. It runs application-level health probes on top of systemd's process supervision, restarts on failure with exponential backoff, and fails the pipeline cleanly when a critical backend stays down.

The orchestrator detects llama.cpp crashes, restarts the unit, observes recovery, and emits `HealthChanged` events. The dialogue path stalls during outage and resumes when the service recovers.

## Out of scope

- Whisper.cpp / Piper supervision (same code path, but those services arrive in M5 / M3 — supervisor is generic, just gets new instances configured).
- A graphical service-management UI. The HTTP `/status` endpoint exposes service states; that's enough for M2.

## New deps

| Lib | Version | Purpose |
|---|---|---|
| libsystemd-dev | 252+ | sd-bus client API |

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)
```

## Step 1 — sd-bus client wrapper

**Files:**
- `src/supervisor/sdbus_client.hpp` — RAII wrapper over `sd_bus*`.
- `src/supervisor/sdbus_client.cpp`

**Operations needed against `org.freedesktop.systemd1`** (user-bus by default):
- `StartUnit(name, "replace")`
- `StopUnit(name, "replace")`
- `RestartUnit(name, "replace")`
- `GetUnit(name)` → object path
- Read property `ActiveState` (loaded/active/inactive/failed) and `SubState` (running/start/stop/dead/...)
- Subscribe to `PropertiesChanged` signal on the unit's path (so we react to systemd-driven state changes without polling)

**API:**
```cpp
class SdBus {
public:
    enum class BusKind { User, System };
    explicit SdBus(BusKind kind);
    ~SdBus();

    // Synchronous unit ops. All return std::expected-shaped result via
    // std::variant<Ok, Error> for C++20 compatibility (see config/config.hpp).
    Result<void> start_unit(std::string_view unit);
    Result<void> stop_unit(std::string_view unit);
    Result<void> restart_unit(std::string_view unit);

    struct UnitState {
        std::string active_state;   // active, inactive, failed, ...
        std::string sub_state;      // running, dead, start-pre, ...
        std::uint64_t inactive_enter_timestamp_usec = 0;
    };
    Result<UnitState> unit_state(std::string_view unit);

    // Subscribe to property changes for a unit. Callback fires from the
    // sd-bus event-loop thread.
    using PropertyCallback = std::function<void(std::string_view unit,
                                                const UnitState&)>;
    void subscribe_unit(std::string_view unit, PropertyCallback cb);

    void run_once(std::chrono::milliseconds timeout);
};
```

**Bus kind:** default to user bus (`systemctl --user`). Document the system-bus path; honor `cfg.supervisor.bus_kind`.

**Tests** (`tests/test_sdbus_client.cpp`):
- Spin a tiny test unit (`/tmp/lclva-test.service` running `sleep 30`), drop it into `~/.config/systemd/user/`, daemon-reload, then exercise start/stop/restart/state.
- Gated behind `LCLVA_SDBUS_TEST=1` env var.

## Step 2 — Supervisor state machine

**Files:**
- `src/supervisor/supervisor.hpp`
- `src/supervisor/supervisor.cpp`

**Per-service states:**
```
NotConfigured → Starting → Healthy
                              ↓
                          Degraded → Unhealthy → Restarting → Starting
                                                       ↓
                                                  Disabled
```

- `Starting`: systemd reports `ActiveState=active SubState=running` but app-level probe hasn't succeeded yet.
- `Healthy`: probe last succeeded within `health_window_ms`.
- `Degraded`: one probe failed but unit still active. Tolerate up to `degraded_max_failures`.
- `Unhealthy`: more than `degraded_max_failures` consecutive probe failures, or systemd reports unit failed.
- `Restarting`: orchestrator told systemd to restart; waiting for `Starting` again.
- `Disabled`: exhausted `max_restarts_per_minute`; pipeline-fail or stay disabled per config.

**Behaviour:**
- Periodic probe every `probe_interval_ms` (default 5000 ms when Healthy, 1000 ms when Degraded).
- On `Unhealthy`: call `restart_unit`. Apply exponential backoff: probe at intervals `restart_backoff_ms` until either Healthy or Disabled.
- Emit `HealthChanged` events on every transition.
- Rate limit: count restarts in a rolling 60-second window.

**API:**
```cpp
struct ServiceConfig {
    std::string name;             // logical, e.g. "llm"
    std::string unit;             // systemd unit, e.g. "lclva-llama.service"
    std::function<bool()> probe;  // returns true if app-level healthy
    bool fail_pipeline_if_down = true;
    std::chrono::milliseconds probe_interval_healthy{5000};
    std::chrono::milliseconds probe_interval_degraded{1000};
    int degraded_max_failures = 3;
    std::vector<std::chrono::milliseconds> restart_backoff{
        500ms, 1000ms, 2000ms, 5000ms, 10000ms};
    int max_restarts_per_minute = 5;
};

class Supervisor {
public:
    Supervisor(SdBus& bus, event::EventBus& evbus, metrics::Registry& metrics);
    ~Supervisor();

    void register_service(ServiceConfig cfg);
    void start();      // begins probing
    void stop();       // halts probes; does not stop the units themselves
    [[nodiscard]] std::vector<ServiceState> states() const; // for /status
};
```

`ServiceState` exposed in the `/status` JSON:
```json
"services": [
  {"name":"llm","unit":"lclva-llama.service","state":"healthy","last_probe_ms":421,"restarts":0},
  {"name":"stt","unit":"lclva-whisper.service","state":"unconfigured"}
]
```

## Step 3 — LLM keep-alive

llama.cpp idle for ~minutes can trigger model offload (depending on flags). Even if it doesn't, we want a periodic warm-up to keep the KV-cache hot.

**Implementation:** A timer on the supervisor thread fires every `cfg.llm.keep_alive_interval_seconds`. When the LLM is `Healthy` and no real turn is in progress, submit a 1-token completion via `LlmClient`. Hide it from metrics under `voice_llm_keepalive_total` so it doesn't pollute the per-turn latency histograms.

**Risk:** keep-alive races with a real user turn. Guard: if the FSM is not in `Listening`, skip this tick.

## Step 4 — Pipeline-level fail conditions

When a service is `Disabled` and `fail_pipeline_if_down = true`:
- Publish `ErrorEvent{ component="supervisor", message="llm disabled" }`.
- The Dialogue Manager (M1) refuses new turns: if `FinalTranscript` arrives while LLM is disabled, transition to `Listening` and log a single warning.
- Optionally emit a one-time spoken error if `cfg.ux.speak_errors=true` (currently false; debug-only).

## Step 5 — systemd unit file scaffolding

**Files (in repo, not installed by build):**
- `packaging/systemd/lclva.service` — orchestrator unit
- `packaging/systemd/lclva-llama.service` — llama.cpp server
- `packaging/systemd/lclva-whisper.service` — whisper.cpp streaming wrapper (filled in for M5; placeholder ExecStart for M2)
- `packaging/systemd/lclva-piper.service` — Piper (filled in for M3; placeholder ExecStart for M2)

User-mode is the default. Install location: `~/.config/systemd/user/`.

**M2 ships `lclva-llama.service` only**; the others land when their milestones do. A symlink-into-place script (`scripts/install-units.sh`) is M8 work but the units themselves exist now so the README setup section can reference them.

## Step 6 — Config extension

```yaml
supervisor:
  bus_kind: user                 # user | system
  probe_interval_healthy_ms: 5000
  probe_interval_degraded_ms: 1000
  degraded_max_failures: 3
  restart_backoff_ms: [500, 1000, 2000, 5000, 10000]
  max_restarts_per_minute: 5
  fail_pipeline_if_llm_down: true
  fail_pipeline_if_stt_down: true     # consumed in M5
  allow_tts_disabled: true            # consumed in M3

llm:
  unit: "lclva-llama.service"
  keep_alive_interval_seconds: 60
```

## Test plan

| Test | Scope |
|---|---|
| `test_sdbus_client.cpp` | start / stop / restart / read state on a real test unit (gated) |
| `test_supervisor.cpp` | mock SdBus + mock probe; assert state transitions, backoff schedule, rate-limit |
| `test_keep_alive.cpp` | timer fires only in Listening; not during turns |
| Manual integration | kill llama.cpp, observe restart, observe recovery, log diagnostics |

## Acceptance

1. With `lclva-llama.service` running, `Supervisor` reports `state=healthy`. `/status` returns the same.
2. `pkill -9 llama-server`: systemd reports unit `failed`; supervisor transitions `Healthy → Unhealthy → Restarting`; new instance reaches `Healthy` within `restart_backoff_ms[0]+probe_interval`.
3. With llama.cpp deliberately broken (e.g., model file deleted): supervisor cycles through backoff; after `max_restarts_per_minute` exhausted, transitions to `Disabled` and emits `ErrorEvent`.
4. Restart count exposed via `voice_service_restarts_total{service="llm"}`.
5. Keep-alive ticks fire every `keep_alive_interval_seconds` while idle; do not fire during active turns.

## Risks specific to M2

| Risk | Mitigation |
|---|---|
| sd-bus event-loop blocks | Run sd-bus on its own thread; never call from the realtime path |
| Restart storms (backoff edge cases) | Hard cap by rolling-window rate limit |
| Test fixtures for sd-bus require a real systemd | Gate tests behind env var; document local setup; use mock SdBus for unit-test path |
| User-bus vs system-bus mode confusion | Single config knob; document in README; default to user bus (no privileges needed) |
| Keep-alive racing with real turns | Skip keep-alive unless FSM is `Listening`; cancellation token passed through |

## Time breakdown

| Step | Estimate |
|---|---|
| 1 sd-bus client | 1.5 days |
| 2 Supervisor state machine | 2 days |
| 3 Keep-alive | 0.5 day |
| 4 Pipeline-fail wiring | 0.5 day |
| 5 systemd unit files | 0.5 day |
| 6 Config + tests | 1 day |
| **Total** | **~5–6 days = 1 week** |
