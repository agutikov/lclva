#pragma once

#include "config/config.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace acva::orchestrator {

namespace detail {

// Parsed view of a single line of `pactl list short modules` whose
// module name is `module-echo-cancel`. Exposed for unit testing —
// the popen + line-walking belongs to system_aec.cpp.
struct EchoCancelModule {
    std::string id;          // numeric module id (always non-empty on success)
    std::string source_name; // empty iff `source_name=` absent from args
    std::string sink_name;   // empty iff `sink_name=` absent from args
};

std::optional<EchoCancelModule>
    parse_echo_cancel_module_line(std::string_view line);

} // namespace detail

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
// Failure modes:
//   * `use_system_aec=false`: no-op; `active()` is false; never errors.
//   * `use_system_aec=true` and one of:
//       - pactl missing / no PulseAudio / module load fails, OR
//       - existing module-echo-cancel found but its args don't expose
//         source_name= / sink_name= (we can't reliably name the
//         PA-side nodes to route through),
//     → `startup_error()` returns a populated message and `active()`
//     stays false. Caller (main) must check and exit non-zero rather
//     than fall back silently to the default audio path. Silent
//     fallback was the M6B Step 4.2 false-pass mode (2026-05-04):
//     audio went around the AEC and gate 1 reported 33/min.
// Never throws.
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
    // unload it. False when we reused an existing module without
    // taking ownership.
    [[nodiscard]] bool owns_module() const noexcept { return owned_; }

    [[nodiscard]] std::string_view module_id() const noexcept { return module_id_; }

    // Populated when `use_system_aec: true` but setup failed in a way
    // that prevents safe operation. main() checks this immediately
    // after construction and exits non-zero.
    [[nodiscard]] const std::optional<std::string>&
        startup_error() const noexcept { return startup_error_; }

private:
    bool                       active_ = false;
    bool                       owned_  = false;
    std::string                module_id_;
    std::string                source_name_;
    std::string                sink_name_;
    std::optional<std::string> startup_error_;
};

} // namespace acva::orchestrator
