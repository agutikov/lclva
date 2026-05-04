#include "orchestrator/bootstrap.hpp"

#include "config/paths.hpp"
#include "log/log.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace acva::orchestrator {

std::variant<LoadedConfig, config::LoadError>
load_and_resolve_config(const std::filesystem::path& cli_config_path,
                         bool stdin_mode) {
    auto cp = config::resolve_config_path(cli_config_path.string());
    if (auto* err = std::get_if<config::LoadError>(&cp)) {
        return *err;
    }
    auto path = std::get<std::filesystem::path>(std::move(cp));

    auto load = config::load_from_file(path);
    if (auto* err = std::get_if<config::LoadError>(&load)) {
        return *err;
    }
    auto cfg = std::get<config::Config>(std::move(load));

    // --stdin still implies "fake driver off", since the user is the
    // event source. Without this the synthetic driver would race the
    // typed input.
    if (stdin_mode) {
        cfg.pipeline.fake_driver_enabled = false;
    }

    // Resolve the SQLite path: empty / relative → under XDG_DATA_HOME.
    // Mutates cfg in place so anything that re-reads cfg.memory.db_path
    // later (e.g. /status, log lines) sees the canonical absolute path.
    cfg.memory.db_path =
        config::resolve_data_path(cfg.memory.db_path, "acva.db").string();

    // M4 — resolve the Silero VAD model path against XDG_DATA_HOME.
    // Empty → ${XDG_DATA_HOME}/acva/models/silero/silero_vad.onnx (the
    // path scripts/download-vad.sh writes to). If the file isn't
    // there, AudioPipeline catches the load failure and disables VAD
    // with a warning — so leaving model_path unset is safe.
    if (cfg.vad.model_path.empty()) {
        const auto resolved = config::resolve_data_path(
            "", "models/silero/silero_vad.onnx");
        if (std::filesystem::exists(resolved)) {
            cfg.vad.model_path = resolved.string();
        }
    } else {
        cfg.vad.model_path = config::resolve_data_path(
            cfg.vad.model_path, "silero/silero_vad.onnx").string();
    }

    return LoadedConfig{std::move(cfg), std::move(path)};
}

void install_alsa_sidestep(const config::AudioConfig& audio) {
    if (!audio.skip_alsa_full_probe) return;

    char tmpl[] = "/tmp/acva-asound.XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        log::warn("main",
            "skip_alsa_full_probe=true but mkstemp failed; "
            "falling back to system asound.conf — startup may stall "
            "on alsa-pipewire-jack");
        return;
    }
    constexpr std::string_view kMinimalConf =
        "pcm.!default { type pulse }\n"
        "ctl.!default { type pulse }\n";
    (void)::write(fd, kMinimalConf.data(), kMinimalConf.size());
    ::close(fd);
    ::setenv("ALSA_CONFIG_PATH", tmpl, /*overwrite=*/1);
    log::info("main", fmt::format(
        "ALSA_CONFIG_PATH={} (skip_alsa_full_probe=true; "
        "PortAudio probe restricted to default → pulse)", tmpl));
}

VramMonitor::VramMonitor(const config::LoggingConfig& logging) {
    if (logging.vram_monitor_interval_ms == 0) return;

    thread_ = std::thread([this,
                            interval_ms = logging.vram_monitor_interval_ms,
                            threshold_mib = logging.vram_low_threshold_mib]{
        const auto interval = std::chrono::milliseconds(interval_ms);
        const auto probe = []() -> std::pair<long, long> {
            FILE* p = ::popen(
                "nvidia-smi --query-gpu=memory.used,memory.free "
                "--format=csv,noheader,nounits 2>/dev/null", "r");
            if (!p) return {-1, -1};
            long used = -1, free_ = -1;
            if (std::fscanf(p, " %ld , %ld", &used, &free_) != 2) {
                used = -1; free_ = -1;
            }
            ::pclose(p);
            return {used, free_};
        };
        auto first = probe();
        if (first.first < 0) {
            log::warn("vram",
                "nvidia-smi unavailable — VRAM monitor disabled");
            return;
        }
        // Edge-triggered logging: emit `vram_low` only on the
        // transition from healthy → low and `vram_recovered` on
        // low → healthy, so a steady-state run is silent. If the
        // first probe is already low, that counts as a transition
        // and we emit once.
        bool low = false;
        while (!stop_.load(std::memory_order_acquire)) {
            const auto v = probe();
            if (v.first >= 0) {
                const bool now_low =
                    v.second < static_cast<long>(threshold_mib);
                if (now_low && !low) {
                    log::event("vram", "vram_low", event::kNoTurn,
                        {{"used_mib", std::to_string(v.first)},
                         {"free_mib", std::to_string(v.second)},
                         {"threshold_mib", std::to_string(threshold_mib)}});
                    low = true;
                } else if (!now_low && low) {
                    log::event("vram", "vram_recovered", event::kNoTurn,
                        {{"used_mib", std::to_string(v.first)},
                         {"free_mib", std::to_string(v.second)},
                         {"threshold_mib", std::to_string(threshold_mib)}});
                    low = false;
                }
            }
            std::this_thread::sleep_for(interval);
        }
    });
}

VramMonitor::~VramMonitor() {
    if (thread_.joinable()) {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }
}

} // namespace acva::orchestrator
