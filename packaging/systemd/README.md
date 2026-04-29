# systemd unit files

Per-user systemd units for the lclva runtime stack. Copy to `~/.config/systemd/user/` and customize before starting.

## Files

| File | Role | Notes |
|---|---|---|
| `lclva-llama.service`   | LLM backend (llama.cpp) | listens on `127.0.0.1:8081`. CUDA build expected. |
| `lclva-whisper.service` | STT backend (whisper.cpp) | listens on `127.0.0.1:8082`. **Placeholder until M5** — the streaming wrapper replaces `ExecStart`. |
| `lclva-piper.service`   | TTS backend (Piper)    | listens on `127.0.0.1:8083`. **Placeholder until M3** — needs `piper-server.py`. |
| `lclva.service`         | Orchestrator           | listens on `127.0.0.1:9876` (control plane). Depends on the three backends. |
| `lclva.target`          | Convenience target     | brings up all four units in dependency order. |

## Path conventions used inside the units

The units use `%h` (systemd specifier expanding to the user's home) for portability. They assume:

```
%h/.local/opt/llama.cpp/build/bin/llama-server
%h/.local/opt/whisper.cpp/build/bin/whisper-server
%h/.local/opt/lclva/piper-server.py
%h/.local/bin/lclva
%h/.local/share/lclva/models/qwen2.5-7b-instruct-q4_k_m.gguf
%h/.local/share/lclva/models/ggml-small.bin
%h/.local/share/lclva/voices/
%h/.config/lclva/config.yaml
```

Edit any path in the `ExecStart=` line if your layout differs.

## Install (per-user)

```sh
mkdir -p ~/.config/systemd/user
cp packaging/systemd/lclva-*.service packaging/systemd/lclva.target ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now lclva.target
```

To survive logout on a headless server: `sudo loginctl enable-linger "$USER"`.

## Install (system-wide, for headless / shared hosts)

1. Move binaries to `/opt/lclva/` (or similar root-owned path) and edit each `ExecStart` line accordingly.
2. Create a dedicated `lclva` system user and add `User=lclva` and `Group=lclva` under each `[Service]` section.
3. Replace every `%h` with the absolute home path (e.g., `/var/lib/lclva`).
4. Copy units to `/etc/systemd/system/` instead of `~/.config/systemd/user/`.
5. `sudo systemctl daemon-reload && sudo systemctl enable --now lclva.target`.
6. In the orchestrator's YAML config, set `supervisor.bus_kind: system`.

The orchestrator's sd-bus client picks up `bus_kind` and connects to the appropriate bus.

## Quick reference

```sh
# bring everything up
systemctl --user start lclva.target

# health checks
curl -sS http://127.0.0.1:8081/health   # llama
curl -sS http://127.0.0.1:8082/health   # whisper
curl -sS http://127.0.0.1:8083/health   # piper
curl -sS http://127.0.0.1:9876/status   # orchestrator

# logs
journalctl --user -fu lclva.service
journalctl --user -fu lclva-llama -fu lclva-whisper -fu lclva-piper

# stop the stack
systemctl --user stop lclva.target

# disable autostart
systemctl --user disable lclva.target lclva.service \
  lclva-llama.service lclva-whisper.service lclva-piper.service
```
