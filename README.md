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

## Setting up the runtime services with systemd

The orchestrator does **not** fork its model backends — it manages them as systemd units via sd-bus. You install the units once, then `systemctl --user start lclva-llama.service` (etc.) brings each backend up. The orchestrator's supervisor takes over from there: probes health, restarts on failure, exposes state via `/status`.

We default to **per-user systemd** (`systemctl --user`). It needs no privileges, doesn't pollute the system unit namespace, and works with the same sd-bus API the orchestrator uses. System-wide units are supported for shared workstations; the layout is identical except for the install path and `--user` becoming root.

### 1. Decide where the binaries and models live

Convention used below — adjust as you like:

```
~/.local/opt/llama.cpp/build/bin/llama-server     # built by you
~/.local/opt/whisper-server/                      # ours; M5 deliverable
~/.local/opt/piper/piper                          # release tarball
~/.local/share/lclva/models/qwen2.5-7b-instruct-q4_k_m.gguf
~/.local/share/lclva/models/ggml-small.bin        # whisper
~/.local/share/lclva/voices/en_US-amy-medium.onnx
~/.local/share/lclva/voices/en_US-amy-medium.onnx.json
```

Create the dirs:
```sh
mkdir -p ~/.local/opt ~/.local/share/lclva/{models,voices,db}
```

Download the models (sizes from the README dependency table):

```sh
# Qwen2.5-7B-Instruct GGUF Q4_K_M (~4.5 GB)
curl -L -o ~/.local/share/lclva/models/qwen2.5-7b-instruct-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf

# Whisper small multilingual (~244 MB)
curl -L -o ~/.local/share/lclva/models/ggml-small.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin

# Silero VAD (~2 MB)
curl -L -o ~/.local/share/lclva/models/silero_vad.onnx \
  https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx

# A Piper voice (~30 MB) — repeat per language you want
mkdir -p ~/.local/share/lclva/voices
curl -L -o ~/.local/share/lclva/voices/en_US-amy-medium.onnx \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/medium/en_US-amy-medium.onnx
curl -L -o ~/.local/share/lclva/voices/en_US-amy-medium.onnx.json \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/medium/en_US-amy-medium.onnx.json
```

### 2. Write the unit files

Per-user units live in `~/.config/systemd/user/`. Create the directory if it doesn't exist:

```sh
mkdir -p ~/.config/systemd/user
```

#### `~/.config/systemd/user/lclva-llama.service`

```ini
[Unit]
Description=lclva — llama.cpp (LLM backend)
After=network.target

[Service]
Type=simple
ExecStart=%h/.local/opt/llama.cpp/build/bin/llama-server \
  --host 127.0.0.1 \
  --port 8081 \
  --model %h/.local/share/lclva/models/qwen2.5-7b-instruct-q4_k_m.gguf \
  --ctx-size 8192 \
  --n-gpu-layers 999 \
  --threads 4 \
  --metrics
Restart=on-failure
RestartSec=2
TimeoutStartSec=120
# Resource limits — the LLM is the heaviest component.
MemoryMax=10G

[Install]
WantedBy=default.target
```

Notes:
- `--n-gpu-layers 999` offloads everything to GPU (your 4060 has the headroom for Q4_K_M).
- `--metrics` enables the OpenAI-compat `/metrics` endpoint that prometheus-cpp can scrape later.
- The orchestrator probes `/health` on this port; matches `cfg.llm.base_url` in the YAML.

#### `~/.config/systemd/user/lclva-whisper.service`

Until M5 ships the streaming wrapper, you can run the upstream `whisper.cpp` server example directly:

```ini
[Unit]
Description=lclva — whisper.cpp (STT backend)
After=network.target

[Service]
Type=simple
ExecStart=%h/.local/opt/whisper.cpp/build/bin/whisper-server \
  --host 127.0.0.1 \
  --port 8082 \
  --model %h/.local/share/lclva/models/ggml-small.bin \
  --threads 6 \
  --processors 1
Restart=on-failure
RestartSec=2
TimeoutStartSec=60

[Install]
WantedBy=default.target
```

(Once M5 lands, replace `ExecStart` with the streaming wrapper at `~/.local/opt/whisper-server/whisper-server`.)

#### `~/.config/systemd/user/lclva-piper.service`

```ini
[Unit]
Description=lclva — Piper (TTS backend)
After=network.target

[Service]
Type=simple
ExecStart=%h/.local/opt/lclva/piper-server.py \
  --host 127.0.0.1 \
  --port 8083 \
  --voices-dir %h/.local/share/lclva/voices
Restart=on-failure
RestartSec=2
TimeoutStartSec=60
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=default.target
```

(`piper-server.py` is the wrapper M3 ships — see `packaging/piper-server/`. Until M3 lands, the unit is a placeholder.)

#### `~/.config/systemd/user/lclva.service`

```ini
[Unit]
Description=lclva — Long Conversation Local Voice Agent
After=lclva-llama.service lclva-whisper.service lclva-piper.service
Wants=lclva-llama.service lclva-whisper.service lclva-piper.service

[Service]
Type=simple
ExecStart=%h/.local/bin/lclva --config %h/.config/lclva/config.yaml
Restart=on-failure
RestartSec=2
StandardOutput=journal
StandardError=journal
# Real-time scheduling for the audio thread (optional, but reduces underruns).
LimitRTPRIO=20
LimitMEMLOCK=infinity

[Install]
WantedBy=default.target
```

#### `~/.config/systemd/user/lclva.target` (convenience)

```ini
[Unit]
Description=lclva voice stack (orchestrator + backends)
Wants=lclva-llama.service lclva-whisper.service lclva-piper.service lclva.service
After=lclva-llama.service lclva-whisper.service lclva-piper.service

[Install]
WantedBy=default.target
```

### 3. Reload, enable, start

```sh
systemctl --user daemon-reload

# Enable to start on login (optional — skip if you prefer manual control).
systemctl --user enable lclva-llama.service lclva-whisper.service lclva-piper.service lclva.service

# Bring the whole stack up.
systemctl --user start lclva.target
```

If you don't run a graphical session and want services to keep running after logout, enable lingering once:

```sh
sudo loginctl enable-linger "$USER"
```

### 4. Verify

```sh
# All four units active?
systemctl --user status lclva.target

# Health checks on each backend.
curl -sS http://127.0.0.1:8081/health     # llama.cpp
curl -sS http://127.0.0.1:8082/health     # whisper
curl -sS http://127.0.0.1:8083/health     # piper

# Orchestrator status (FSM, supervisor states, queue depths).
curl -sS http://127.0.0.1:9876/status | python -m json.tool

# Prometheus metrics — useful for hooking up Grafana later.
curl -sS http://127.0.0.1:9876/metrics | head -40
```

### 5. Logs

systemd captures stdout/stderr from each unit into journald. View them:

```sh
# Live tail of the orchestrator.
journalctl --user -fu lclva.service

# Last 500 lines of llama.cpp.
journalctl --user -n 500 -u lclva-llama.service

# Across all units.
journalctl --user -fu lclva-llama -fu lclva-whisper -fu lclva-piper -fu lclva
```

### 6. Stopping / restarting

```sh
# Stop everything.
systemctl --user stop lclva.target

# Restart just the LLM (e.g., after a model swap).
systemctl --user restart lclva-llama.service

# Disable everything from auto-starting on login.
systemctl --user disable lclva.target lclva.service \
  lclva-llama.service lclva-whisper.service lclva-piper.service
```

### Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `lclva-llama.service` enters `failed` immediately | `llama-server` couldn't load the model (path / OOM / GPU not visible) | `journalctl --user -u lclva-llama -n 100` shows the underlying error. Common fixes: check model path, lower `--n-gpu-layers`, free VRAM. |
| `lclva.service` exits with "control server: failed to bind" | Port already in use | Another `lclva` is running, or the port is taken. `ss -tlnp \| grep 9876` to confirm. |
| Orchestrator's `/status` shows `state=unconfigured` for `llm` | Supervisor can't reach `lclva-llama.service` over sd-bus | Check `cfg.llm.unit` matches the unit filename. `busctl --user list \| grep systemd1`. |
| Audio crackles or no sound | systemd unit can't access the sound server | If you launched lclva from an SSH session without a graphical login, PulseAudio/PipeWire may not be reachable. Either use linger + autospawn, or run lclva interactively from a desktop session. |
| Logs say "permission denied: /dev/snd" | Missing audio group membership | `sudo usermod -aG audio "$USER"` and re-login. |
| `--n-gpu-layers` ignored | NVIDIA driver / CUDA not loaded for the user session | `nvidia-smi` from the same login that runs the unit. May need `Environment=CUDA_VISIBLE_DEVICES=0` in the unit. |

### System-wide install (alternative)

If you want services to run regardless of user session — useful for a headless server:

1. Move binaries to `/opt/lclva/` (or similar root-owned path).
2. Place units in `/etc/systemd/system/` instead of `~/.config/systemd/user/`.
3. Add a dedicated `lclva` system user; `User=lclva` in each unit.
4. `sudo systemctl daemon-reload && sudo systemctl enable --now lclva.target`.
5. In `cfg.supervisor.bus_kind`, set `system` instead of `user`.

The orchestrator's sd-bus client picks up `bus_kind` and connects to the appropriate bus. Everything else is the same.

## License

See `LICENSE`.
