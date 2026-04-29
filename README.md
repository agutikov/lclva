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
- **Model runtimes run as systemd-managed services** — llama.cpp (LLM), whisper.cpp (STT), Piper (TTS) — the orchestrator interacts with them via sd-bus.
- **Realtime audio path is isolated**: lock-free SPSC ring between the audio callback and the processing thread; no blocking, no allocation, no I/O on the audio thread.
- **Cancellation is structural**: every operation carries a turn ID; barge-in invalidates the turn ID and stale work is rejected at every queue boundary.

See `plans/project_design.md` for the complete architecture.

## Target Stack

| Concern           | Choice                                       |
|-------------------|----------------------------------------------|
| Language          | C++23 (no modules, no Cobalt)                |
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
| M5 | Streaming STT + speculation| 2–3 weeks        |
| M6 | AEC / NS / AGC             | 1–2 weeks        |
| M7 | Barge-in                   | 1 week           |
| M8 | Production hardening       | 2 weeks          |

Total: **~14–16 weeks** for a single competent C++ developer to MVP.

## Success Criteria for MVP

- End-to-end P50 latency ≤ 2 s, P95 ≤ 3.5 s on target hardware
- 4-hour soak test: no crashes, bounded memory growth, stable latency percentiles
- Barge-in (speaker mode + AEC, primary UX): ≥ 90 % correct cancellation within 400 ms; headphone mode ≥ 95 % within 300 ms
- Clean recovery from mid-session crashes

## Hardware Target

- GPU: RTX 4060 8 GB
- CPU: modern x86_64 (≥ 8 cores)
- OS: Linux (Manjaro/Arch primary, Ubuntu LTS secondary)
- Audio: USB or built-in mic + speakers/headphones

## Dependencies

### Build-time (compile the orchestrator)

| Dependency           | Min version | Used for                              | Header-only? |
|----------------------|-------------|---------------------------------------|--------------|
| C++ compiler         | gcc ≥ 13 / clang ≥ 17 | C++20                       | —            |
| CMake                | 3.25        | Build system + presets                | —            |
| pkg-config           | any         | Locating system libs                  | —            |
| Boost                | 1.83+       | `Boost.Asio` (async runtime)          | no (Asio is mostly header-only but Boost ships built libs we may link) |
| libcurl              | 7.80+       | LLM SSE streaming                     | no           |
| cpp-httplib          | 0.15+       | Non-streaming HTTP (health, TTS)      | yes (vendored) |
| glaze                | 4.0+        | JSON + YAML config / IPC payloads     | yes (vendored) |
| spdlog               | 1.13+       | Structured logging (JSON sink)        | optional header-only build |
| prometheus-cpp       | 1.2+        | `/metrics` endpoint                   | no           |
| opentelemetry-cpp    | 1.16+       | OTLP traces (opt-in feature)          | no           |
| libsystemd           | 252+        | sd-bus, journald integration          | no           |
| PortAudio            | 19.7+       | Audio capture/playback                | no           |
| soxr                 | 0.1.3+      | Resampling                            | no           |
| ONNX Runtime         | 1.18+       | Silero VAD inference                  | no           |
| WebRTC APM           | vendored    | AEC / NS / AGC                        | vendored as a CMake submodule |
| SQLite               | 3.42+       | Memory storage (WAL mode)             | no           |
| doctest *or* Catch2  | latest      | Unit tests                            | yes (header-only) |

### Runtime (separate processes, managed by systemd)

| Component            | Notes                                                                |
|----------------------|----------------------------------------------------------------------|
| llama.cpp server     | Built separately. OpenAI-compatible REST + SSE. CUDA build for 4060. |
| whisper.cpp          | Built separately. **Custom streaming HTTP wrapper** required (M5).   |
| Piper TTS            | Built separately. Per-language voice models.                         |
| systemd              | ≥ 252. Required at runtime; orchestrator manages units via sd-bus.   |

### Models (downloaded; not packaged with source)

| Model                          | Size     | Source                                                  |
|--------------------------------|----------|---------------------------------------------------------|
| Qwen2.5-7B-Instruct GGUF Q4_K_M| ~4.5 GB  | huggingface.co (Qwen GGUF community quantizations)      |
| Whisper small (multilingual)   | ~244 MB  | github.com/ggerganov/whisper.cpp/releases (ggml format) |
| Silero VAD ONNX                | ~2 MB    | github.com/snakers4/silero-vad                          |
| Piper voices (per language)    | 30–100 MB each | github.com/rhasspy/piper/releases               |

Total disk footprint with Qwen Q4_K_M + Whisper small + 3 Piper voices: **~5.5 GB**.

### Optional

- **otelcol-contrib** — local OpenTelemetry collector if OTLP traces are enabled. Otherwise OTLP stays disabled and JSON logs are sufficient.
- **NVIDIA driver + CUDA toolkit** — required for llama.cpp GPU build (the only GPU-resident component; Whisper runs on CPU).

## Installing dependencies

The package lists below cover **build-time and runtime dependencies that are reasonably packaged**. Items not in distro repositories (llama.cpp, whisper.cpp, Piper, models) are installed separately — see *Building external services* at the end of this section.

### Manjaro / Arch

```sh
# Toolchain + system libraries from official repos
sudo pacman -S --needed \
  base-devel cmake pkgconf git \
  boost \
  curl \
  systemd-libs \
  portaudio \
  soxr \
  onnxruntime \
  sqlite \
  spdlog \
  nlohmann-json   # transitive; some optional deps still pull it

# AUR (use yay / paru)
yay -S --needed \
  prometheus-cpp \
  opentelemetry-cpp \
  glaze \
  cpp-httplib \
  doctest

# CUDA for the GPU LLM build (skip if you'll download a prebuilt llama.cpp)
sudo pacman -S --needed cuda

# Optional OTel collector for OTLP tracing
yay -S --needed otelcol-contrib-bin
```

### Debian / Ubuntu (24.04 LTS or newer)

```sh
sudo apt update
sudo apt install --no-install-recommends \
  build-essential gcc-13 g++-13 cmake pkg-config git ca-certificates \
  libboost-all-dev \
  libcurl4-openssl-dev \
  libsystemd-dev \
  portaudio19-dev \
  libsoxr-dev \
  libsqlite3-dev \
  libspdlog-dev \
  libonnxruntime-dev   # 24.04+; on 22.04 build from source

# These are not in apt; build from source under ~/src or vendor as submodules:
#   - prometheus-cpp     https://github.com/jupp0r/prometheus-cpp
#   - opentelemetry-cpp  https://github.com/open-telemetry/opentelemetry-cpp
#   - glaze              https://github.com/stephenberry/glaze (header-only)
#   - cpp-httplib        https://github.com/yhirose/cpp-httplib (header-only)

# CUDA (NVIDIA's apt repository, follow upstream instructions for your driver version)
# https://developer.nvidia.com/cuda-downloads

# Optional OTel collector
# Download otelcol-contrib release tarball from
# https://github.com/open-telemetry/opentelemetry-collector-releases/releases
```

Build-from-source notes for Debian:

```sh
# prometheus-cpp
git clone --recurse-submodules https://github.com/jupp0r/prometheus-cpp.git
cmake -S prometheus-cpp -B prometheus-cpp/build \
  -DBUILD_SHARED_LIBS=ON -DENABLE_PUSH=OFF -DENABLE_COMPRESSION=OFF
sudo cmake --build prometheus-cpp/build -j --target install

# opentelemetry-cpp (HTTP OTLP only, keeps deps lean)
git clone --recurse-submodules https://github.com/open-telemetry/opentelemetry-cpp.git
cmake -S opentelemetry-cpp -B opentelemetry-cpp/build \
  -DWITH_OTLP_HTTP=ON -DWITH_OTLP_GRPC=OFF -DBUILD_TESTING=OFF
sudo cmake --build opentelemetry-cpp/build -j --target install
```

### Building external services

The model runtimes are not distro-packaged and the version cadence is fast. The recommended approach is to build them from source under `~/.local/opt/` and reference them from the systemd unit files.

```sh
# llama.cpp (CUDA build)
git clone https://github.com/ggerganov/llama.cpp.git
cmake -S llama.cpp -B llama.cpp/build -DGGML_CUDA=ON -DLLAMA_CURL=ON
cmake --build llama.cpp/build -j --target llama-server
# Resulting binary: llama.cpp/build/bin/llama-server

# whisper.cpp (we'll wrap this with a custom streaming HTTP server in M5)
git clone https://github.com/ggerganov/whisper.cpp.git
cmake -S whisper.cpp -B whisper.cpp/build
cmake --build whisper.cpp/build -j

# Piper (download a release)
mkdir -p ~/.local/opt/piper && cd ~/.local/opt/piper
curl -LO https://github.com/rhasspy/piper/releases/latest/download/piper_linux_x86_64.tar.gz
tar xzf piper_linux_x86_64.tar.gz
```

systemd units that wrap each service ship in `packaging/systemd/` once that directory exists; see `plans/project_design.md` §4.12.

## License

See `LICENSE`.
