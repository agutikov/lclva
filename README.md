# lclva

**Long Conversation Local Voice Agent.** A local, production-grade C++ voice assistant designed for multi-hour conversations on a single workstation. All inference runs locally; no audio or transcripts leave the machine.

## Status

Pre-implementation. The repository currently contains the architecture and project plan. Implementation has not started.

## Goal

A configurable voice assistant that:

- runs entirely on local hardware (RTX 4060 8 GB target)
- speaker mode primary, with AEC for reliable barge-in (interrupt the assistant by speaking)
- supports multilingual conversation with per-utterance language detection
- streams partial STT and starts the LLM speculatively for sub-2-second latency
- holds 30-minute to multi-hour conversations without crashes, leaks, or latency drift
- recovers cleanly from mid-turn crashes
- is observable from day one (structured logs, per-turn traces, Prometheus metrics, opt-in OTLP)

## Design at a Glance

```
Mic → Resample → APM (AEC/NS/AGC) → VAD → Utterance Buffer → STT
   → Dialogue FSM ↔ Memory ↔ LLM
   → SentenceSplitter → TTS → Playback Queue → Resample → Speaker
                                              ↑
                                           Loopback (AEC reference)
```

- **C++ orchestrator** owns audio I/O, dialogue state, memory, cancellation, and observability.
- **Model runtimes run as separate processes** — llama.cpp (LLM), whisper.cpp (STT), Piper (TTS) — supervised by the orchestrator.
- **Realtime audio path is isolated**: lock-free SPSC ring between the audio callback and the processing thread; no blocking, no allocation, no I/O on the audio thread.
- **Cancellation is structural**: every operation carries a turn ID; barge-in invalidates the turn ID and stale work is rejected at every queue boundary.

See `plans/project_design.md` for the complete architecture.

## Target Stack

| Concern           | Choice                                       |
|-------------------|----------------------------------------------|
| Language          | C++20                                        |
| Async             | Boost.Asio                                   |
| Audio backend     | PortAudio + soxr                             |
| AEC/NS/AGC        | WebRTC Audio Processing Module               |
| VAD               | Silero VAD (ONNX Runtime)                    |
| LLM               | llama.cpp + Qwen2.5-7B Q4_K_M                |
| STT               | whisper.cpp (streaming, multilingual)        |
| TTS               | Piper (per-language voice pack, lazy load)   |
| Storage           | SQLite (WAL mode)                            |
| Config / JSON     | glaze (JSON + YAML)                          |
| Logs / metrics    | spdlog + prometheus-cpp                      |
| Tracing (opt-in)  | opentelemetry-cpp (OTLP)                     |
| Process mgmt      | systemd units + sd-bus                       |
| Build             | CMake + presets                              |

## Repository Layout

```
plans/
  project_design.md       full architecture, milestones, risk register
  open_questions.md       unresolved decisions with default assumptions
  architecture_review.md  first review of the original design
  local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md
                          original design draft
CLAUDE.md                 guidance for Claude Code in this repo
README.md
LICENSE
```

## Milestones

| #  | Milestone                  | Approx. duration |
|----|----------------------------|------------------|
| M0 | Skeleton runtime           | 1 week           |
| M1 | LLM + memory               | 1–2 weeks        |
| M2 | Service supervision        | 1 week           |
| M3 | TTS + playback             | 1–2 weeks        |
| M4 | Audio capture + VAD        | 1–2 weeks        |
| M5 | STT                        | 1 week           |
| M6 | AEC / NS / AGC             | 1–2 weeks        |
| M7 | Barge-in                   | 1 week           |
| M8 | Production hardening       | 2 weeks          |

Total: **~14–16 weeks** for a single competent C++ developer to MVP.

## Success Criteria for MVP

- End-to-end P50 latency ≤ 2 s, P95 ≤ 3.5 s on target hardware
- 4-hour soak test: no crashes, bounded memory growth, stable latency percentiles
- Barge-in: ≥ 95 % correct cancellation in headphone mode, ≥ 80 % in speaker mode (with AEC)
- Clean recovery from mid-session crashes

## Hardware Target

- GPU: RTX 4060 8 GB
- CPU: modern x86_64 (≥ 8 cores)
- OS: Linux (Manjaro/Arch primary, Ubuntu LTS secondary)
- Audio: USB or built-in mic + speakers/headphones

## License

See `LICENSE`.
