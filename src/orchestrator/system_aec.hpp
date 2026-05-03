#pragma once

#include "config/config.hpp"

#include <string>

namespace acva::orchestrator {

// RAII helper that routes acva's audio through PipeWire's
// `module-echo-cancel` (system-side AEC) when
// `cfg.apm.use_system_aec` is true.
//
// Why this exists. The in-process WebRTC APM (M6) doesn't cancel
// well on the dev workstation's laptop codec — see docs/aec_report.md
// § 6. PipeWire's `module-echo-cancel` uses the same
// webrtc-audio-processing library but consumes the raw ALSA mic
// stream BEFORE per-application codec DSP, and on this hardware
// gives 25-29 dB of cancellation in the speech band (M6B Step 1
// measurement, 2026-05-03). Letting acva drive that module
// automatically removes the manual `pactl load-module` + env-var
// recipe.
//
// Mechanism:
//   1. On construction, look for an existing module-echo-cancel via
//      `pactl list short modules`. If found, reuse — do NOT take
//      ownership; some other process or the user loaded it and we
//      shouldn't unload from under them.
//   2. Otherwise, run `pactl load-module module-echo-cancel ...`,
//      capture the returned id, mark ourselves as owner.
//   3. Either way, set `PULSE_SINK` and `PULSE_SOURCE` env vars so
//      this process's PulseAudio "default" device routes through
//      the cleaned source/sink. `install_alsa_sidestep` restricts
//      PortAudio to a single `default → pulse` device, so substring
//      matching `cfg.audio.{input,output}_device` against the
//      virtual device names doesn't work — env vars are the path.
//   4. On destruction, unload IFF we loaded it.
//
// Required ordering: construct AFTER `install_alsa_sidestep`,
// BEFORE any PortAudio init (i.e., before tts_stack /
// capture_stack / playback_engine / any demo). The env vars must
// be set before PortAudio's PulseAudio host opens streams.
//
// Failure modes: pactl missing, module load fails, no PulseAudio /
// PipeWire — log a warning and continue with the default audio
// path (active() returns false). Never throws.
class SystemAec {
public:
    explicit SystemAec(const config::ApmConfig& apm);
    ~SystemAec();

    SystemAec(const SystemAec&)            = delete;
    SystemAec& operator=(const SystemAec&) = delete;
    SystemAec(SystemAec&&)                 = delete;
    SystemAec& operator=(SystemAec&&)      = delete;

    // True when the process is now routed through module-echo-cancel
    // (whether we loaded the module or reused an existing one).
    [[nodiscard]] bool active() const noexcept { return active_; }

    // True when WE loaded the module — i.e., the destructor will
    // unload it. False when we reused an existing module.
    [[nodiscard]] bool owns_module() const noexcept { return owned_; }

    [[nodiscard]] std::string_view module_id() const noexcept { return module_id_; }

private:
    bool         active_ = false;
    bool         owned_  = false;
    std::string  module_id_;
    std::string  source_name_;
    std::string  sink_name_;
};

} // namespace acva::orchestrator
