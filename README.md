# lclva

**Long Conversation Local Voice Agent.** A local, production-grade C++ voice assistant designed for multi-hour conversations on a single workstation. All inference runs locally; no audio or transcripts leave the machine.

## Status

In progress. **M0 and M1 complete.** Skeleton runtime, memory layer, sentence splitter, Docker Compose stack, libcurl SSE LLM client, dialogue manager, turn writer, summarizer stub, and JSON-per-line logging are all landed. 90 unit tests passing. The `lclva --stdin` binary drives a real LLM end-to-end against the Compose stack. Next: **M2 — service supervision** (HTTP `/health` probes, dialogue gating). See `plans/milestones/` for per-milestone detail.

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

- **C++ orchestrator** runs as a host CLI binary and owns audio I/O, dialogue state, memory, cancellation, and observability.
- **Model backends run as separate processes** — llama.cpp (LLM), whisper.cpp (STT), Piper (TTS). Default deployment is **Docker Compose** for dev with upstream images verbatim. systemd units are an alternative production path.
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
| Backend deployment| Docker Compose (dev) / systemd (production)  |
| Build             | CMake + presets                              |

## Repository Layout

```
src/                      C++ source (config, dialogue, event, http, log, memory, metrics, pipeline)
tests/                    doctest-based unit tests
config/default.yaml       default runtime config
cmake/                    CMake helpers
third_party/              vendored single-header libs (cpp-httplib)
plans/
  project_design.md       full architecture, milestones, risk register
  open_questions.md       unresolved decisions with default assumptions
  milestones/m{0..8}_*.md per-milestone implementation plans
  architecture_review.md  first review of the original design
  local_voice_ai_orchestrator_mvp_cpp_architecture_2026.md
                          original design draft
packaging/
  compose/                docker-compose.yml + local Dockerfiles for whisper/piper + fetch-assets.sh
  systemd/                per-user systemd units (alternative production path)
compose.yaml              top-level compose shim that include:s packaging/compose/docker-compose.yml
.env.example              env override template (LCLVA_MODELS_DIR, LCLVA_LLM_MODEL, ...)
CMakeLists.txt, CMakePresets.json
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

### Runtime (model backends — separate processes)

| Component            | Notes                                                                |
|----------------------|----------------------------------------------------------------------|
| llama.cpp server     | OpenAI-compatible REST + SSE. CUDA build for 4060. Upstream image: `ghcr.io/ggml-org/llama.cpp:server-cuda`. |
| whisper.cpp server   | Upstream `whisper-server`. Request/response transcription through M4. Upstream publishes no server image, so the Compose stack builds it locally from `packaging/compose/whisper/Dockerfile` (pinned to `v1.8.4`). M5 picks one of: custom streaming wrapper, [Speaches](https://github.com/speaches-ai/speaches) (faster-whisper, OpenAI Realtime API), or defer streaming past MVP. |
| Piper TTS            | Upstream `python -m piper.http_server`. Built locally from `packaging/compose/piper/Dockerfile` (pinned to piper-tts 1.4.2). One process per language, routed by URL. |
| Docker Engine ≥ 26   | Default deployment in dev. Compose v2 built-in.                      |
| systemd ≥ 252        | Optional alternative for production. Required only on the systemd path; otherwise unused. |

### Models (downloaded; not packaged with source)

| Model                          | Size     | Source                                                  |
|--------------------------------|----------|---------------------------------------------------------|
| Qwen2.5-7B-Instruct GGUF Q4_K_M| ~4.4 GB (split 2 shards) | huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF |
| Whisper small (multilingual)   | ~466 MB  | huggingface.co/ggerganov/whisper.cpp                    |
| Silero VAD ONNX                | ~2 MB    | github.com/snakers4/silero-vad                          |
| Piper voices (per language)    | 30–100 MB each | huggingface.co/rhasspy/piper-voices               |

Total disk footprint with Qwen Q4_K_M + Whisper small + 1 Piper voice: **~5.0 GB**.

The `packaging/compose/fetch-assets.sh` helper downloads the LLM, Whisper model, and one Piper voice to the standard host paths in one shot. Silero VAD is fetched lazily by the orchestrator once M4 lands.

### Optional

- **otelcol-contrib** — local OpenTelemetry collector if OTLP traces are enabled. Otherwise OTLP stays disabled and JSON logs are sufficient.
- **NVIDIA Container Toolkit ≥ 1.14** — required for the Compose / GPU passthrough path (the default dev path). Skip if running backends as systemd units with a host CUDA install.
- **NVIDIA driver + CUDA toolkit** — needed if you build llama.cpp from source for the systemd path. Not required if using the upstream Compose image.

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

## Quickstart with Docker Compose (dev — recommended)

Default for development. Backends run as Compose containers; `lclva` runs on the host as a CLI binary. A top-level `compose.yaml` re-includes `packaging/compose/docker-compose.yml`, so all `docker compose ...` commands work from the repository root.

llama.cpp uses the upstream `ghcr.io/ggml-org/llama.cpp:server-cuda` image verbatim. Whisper and Piper are built locally from minimal Dockerfiles under `packaging/compose/{whisper,piper}/` because upstream does not publish HTTP-server images for either project. Both Dockerfiles are short (≤ 30 lines) and pin to release tags / package versions.

### 1. Prerequisites

- Docker Engine ≥ 26 (Compose v2 built-in). Podman ≥ 4 with `podman-compose` works as well.
- **NVIDIA Container Toolkit** ≥ 1.14 for GPU passthrough to llama.cpp.
  - Arch / Manjaro: `sudo pacman -S nvidia-container-toolkit && sudo systemctl restart docker`
  - Debian / Ubuntu: follow the upstream NVIDIA Container Toolkit install guide.
  - Verify: `docker run --rm --gpus all nvidia/cuda:12.6.0-base-ubuntu24.04 nvidia-smi` prints the GPU table.
  - **CDI spec drift on rolling-release distros:** if the toolkit was generated against a previous driver version, container start fails with `failed to fulfil mount request: open /usr/lib/libnvidia-*.so: no such file or directory`. Regenerate the spec after every NVIDIA driver upgrade:
    ```sh
    sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
    ```
- User in the `docker` group (or rootless Docker).
- Models and voices present on the host under `~/.local/share/lclva/{models,voices}/`. The `fetch-assets.sh` helper below downloads the defaults.

### 2. Download the default model assets

```sh
bash packaging/compose/fetch-assets.sh
```

Idempotent and resumable (curl `--continue-at -`). Default footprint ≈ 5.2 GB:

| File | Size | Source |
|---|---|---|
| `qwen2.5-7b-instruct-q4_k_m-{00001,00002}-of-00002.gguf` | ~4.4 GB | Qwen GGUF on HuggingFace |
| `ggml-small.bin` | ~466 MB | ggerganov/whisper.cpp on HuggingFace |
| `en_US-amy-medium.onnx` (+ `.json`) | ~63 MB | rhasspy/piper-voices on HuggingFace |

Override the destination dirs with `LCLVA_MODELS_DIR` / `LCLVA_VOICES_DIR` (the same env vars compose reads).

### 3. Bring up the backends

```sh
docker compose up -d            # first run pulls ~5 GB of images and builds whisper+piper
docker compose ps               # all 3 services 'healthy' within ~60 s once images cached
```

This starts:

- `llama` on `127.0.0.1:8081` — `ghcr.io/ggml-org/llama.cpp:server-cuda` (CUDA build, GPU passthrough)
- `whisper` on `127.0.0.1:8082` — `lclva/whisper.cpp:server-v1.8.4` (built locally from `packaging/compose/whisper/Dockerfile`)
- `piper` on `127.0.0.1:8083` — `lclva/piper:1.4.2` (built locally from `packaging/compose/piper/Dockerfile`; one voice per service, per-language deployments add more services on adjacent ports)

Health check each backend:

```sh
curl -fsS http://127.0.0.1:8081/health
curl -fsS http://127.0.0.1:8082/health
curl -fsS http://127.0.0.1:8083/health
```

To override host paths or model file selection, copy `.env.example` to `.env` at the repo root and edit. Compose reads `.env` from the working directory by default.

### 4. Run the orchestrator on the host

```sh
cmake --preset dev && cmake --build --preset dev
./build/dev/lclva --config config/default.yaml
# Control plane:
curl -sS http://127.0.0.1:9876/status
curl -sS http://127.0.0.1:9876/metrics
```

Until M1 slice 2 lands, lclva runs in M0 fake-driver mode — it doesn't yet use the Compose backends. Once slice 2 is in, the orchestrator connects to llama / whisper / piper at the URLs in `config/default.yaml`.

### 5. Stop / clean

```sh
docker compose down                  # stop, keep images and volumes
docker compose down -v --rmi all     # nuke everything (including the locally-built images)
```

### 6. Logs

```sh
docker compose logs -f llama         # live tail of one backend
docker compose logs -f               # tail all backends together
```

The orchestrator's logs go to its own stdout (terminal where you launched it).

### Switching to systemd

For unattended / production-style deployments, replace steps 3–6 with the systemd path below. Step 2 (asset download) and step 4 (running the orchestrator) are identical.

---

## Setting up the runtime services with systemd (production-style alternative)

Use this path for unattended workstation deployments where you want services to survive logout, get journald aggregation, and don't want a Docker daemon running. This is also the path that exercises the optional sd-bus extension to the supervisor (gated by `-DLCLVA_ENABLE_SDBUS=ON` from M8 onward).

We default to **per-user systemd** (`systemctl --user`). It needs no privileges, doesn't pollute the system unit namespace. System-wide units are supported for shared workstations; the layout is identical except for the install path and `--user` becoming root.

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

### 2. Install the unit files

Five unit files ship in [`packaging/systemd/`](packaging/systemd/) — see that directory's README for the full reference (path conventions, system-wide install variant, edits you may want to make):

| File | Role |
|---|---|
| `lclva-llama.service`   | LLM backend (llama.cpp) on `127.0.0.1:8081` |
| `lclva-whisper.service` | STT backend (whisper.cpp) on `127.0.0.1:8082` |
| `lclva-piper.service`   | TTS backend (Piper) on `127.0.0.1:8083` |
| `lclva.service`         | Orchestrator (control plane on `127.0.0.1:9876`) |
| `lclva.target`          | Convenience target — brings up all four in order |

Copy them into the user-systemd directory:

```sh
mkdir -p ~/.config/systemd/user
cp packaging/systemd/lclva-*.service packaging/systemd/lclva.target ~/.config/systemd/user/
```

The units use `%h` (systemd specifier expanding to your home directory) and assume the layout from step 1. If your binaries or models live elsewhere, edit each `ExecStart=` line accordingly before continuing.

A few quick notes that wouldn't fit into comments inside the unit files:
- llama.cpp's `--n-gpu-layers 999` offloads every layer to GPU. With Q4_K_M on a 4060 there's headroom.
- llama.cpp's `--metrics` flag enables a Prometheus endpoint on the same port — handy for future scraping.
- The orchestrator probes `/health` on each backend's port; the ports above must match `cfg.llm.base_url`, `cfg.stt.base_url`, `cfg.tts.base_url` in the YAML.

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
