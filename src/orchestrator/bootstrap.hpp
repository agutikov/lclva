#pragma once

#include "config/config.hpp"

#include <atomic>
#include <filesystem>
#include <thread>
#include <variant>

// orchestrator:: — host-side glue that wires the per-subsystem
// modules (audio/, dialogue/, stt/, ...) into a running orchestrator.
// Each builder returns a struct holding the constructed objects with
// RAII teardown so main.cpp can stay thin.
namespace acva::orchestrator {

// Resolve `--config PATH` into an absolute filesystem path, then load
// + post-process the YAML into a Config. Side effects:
//   * cfg.memory.db_path → resolved against XDG_DATA_HOME.
//   * cfg.vad.model_path → resolved against XDG_DATA_HOME if non-empty
//     OR auto-detected at the default Silero path if empty + present.
//   * cfg.pipeline.fake_driver_enabled → forced false in stdin mode.
//
// On any failure (bad path, parse error, validation failure) returns
// the LoadError message; main.cpp prints it to stderr and exits.
struct LoadedConfig {
    config::Config        cfg;
    std::filesystem::path config_path;
};
[[nodiscard]] std::variant<LoadedConfig, config::LoadError>
load_and_resolve_config(const std::filesystem::path& cli_config_path,
                         bool stdin_mode);

// Sidestep PortAudio's full ALSA-PCM probe by writing a minimal
// asound.conf to a tmpfile and pointing ALSA_CONFIG_PATH at it.
// Stops alsa-pipewire-jack glue from deadlocking pipewire's
// thread-loop (~4 minute startup stall). No-op when
// cfg.skip_alsa_full_probe is false. Logs the action via log::info /
// log::warn — call AFTER log::init.
void install_alsa_sidestep(const config::AudioConfig& audio);

// RAII periodic VRAM probe. Spawns a worker that shells nvidia-smi
// every cfg.vram_monitor_interval_ms and emits a `vram` event with
// used / free MiB. Stop via the destructor; safe to construct with
// interval = 0 (disabled — destructor is a no-op).
class VramMonitor {
public:
    explicit VramMonitor(const config::LoggingConfig& logging);
    ~VramMonitor();

    VramMonitor(const VramMonitor&)            = delete;
    VramMonitor& operator=(const VramMonitor&) = delete;
    VramMonitor(VramMonitor&&)                 = delete;
    VramMonitor& operator=(VramMonitor&&)      = delete;

private:
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

} // namespace acva::orchestrator
