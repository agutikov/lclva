# M0 — Skeleton Runtime ✅

**Status:** completed.

## Goal

Stand up the orchestrator process with bus, FSM, observability, and a fake pipeline driver — no real audio or model integration yet. Everything that lands here is foundation that subsequent milestones build on without churn.

## What landed

| Subsystem | Files | Notes |
|---|---|---|
| Build | `CMakeLists.txt`, `CMakePresets.json`, `cmake/Dependencies.cmake`, `cmake/Warnings.cmake` | C++23, Ninja, RelWithDebInfo, `-Werror` |
| Config | `src/config/{config.hpp,config.cpp}` | glaze YAML, validation, `std::variant<Config, LoadError>` (no `std::expected`) |
| Logging | `src/log/{log.hpp,log.cpp}` | spdlog; structured-text format. JSON deferred to M1. |
| Bounded queue | `src/event/queue.hpp` | MPMC, three overflow policies, push/pops/drops counters |
| Events | `src/event/{event.hpp,event.cpp}` | 19 event structs in a `std::variant`; `event_name`/`event_turn` helpers |
| Bus | `src/event/{bus.hpp,bus.cpp}` | typed `subscribe<E>` + `subscribe_all`; per-subscription queue + worker thread |
| Turn | `src/dialogue/{turn.hpp,turn.cpp}` | `TurnId`, `CancellationToken`, `TurnContext`, `TurnFactory` |
| FSM | `src/dialogue/{fsm.hpp,fsm.cpp}` | 8 states; cancellation cascade; outcome observer |
| Metrics | `src/metrics/{registry.hpp,registry.cpp}` | prometheus-cpp wrapper |
| HTTP | `src/http/{server.hpp,server.cpp}` | cpp-httplib (vendored); `/metrics`, `/status`, `/health` |
| Fake pipeline | `src/pipeline/{fake_driver.hpp,fake_driver.cpp}` | drives FSM through a turn lifecycle, including mid-turn barge-in |
| Tests | `tests/test_{queue,bus,fsm,config}.cpp` | 24 tests, all passing |

## What we deliberately did NOT do in M0

- Real audio I/O (M4)
- Real STT/LLM/TTS clients (M1, M3, M5)
- WebRTC APM (M6)
- AEC reference-signal routing (M6)
- systemd integration (M2)
- OTLP traces (M8 / opt-in)
- Tool calling (post-MVP)
- Speculation logic in FSM (M5)

## Acceptance — met

- `ctest` runs 24/24 green.
- `./build/dev/lclva --config config/default.yaml` starts, drives synthetic turns through the FSM, responds on `/metrics` and `/status`, and exits cleanly on SIGINT.
- With `fake_barge_in_probability: 1.0`, observed 5/5 turns end with outcome `interrupted`; FSM correctly bumps turn id and rejects stale events.

## Lessons folded forward

- Glaze 7.x forces `-std=c++23` transitively → C++23 is now the project baseline (no Cobalt, no modules).
- spdlog has no built-in JSON formatter → custom sink in M1 once there's structured turn-context to log.
- prometheus-cpp doesn't expose civetweb's `mg_*` symbols → vendored cpp-httplib for the HTTP control plane.
- gcc 15's `-Wnull-dereference` false-positives on inlined shared_ptr-vector copy-assignment — workaround documented in `event/bus.cpp`.
