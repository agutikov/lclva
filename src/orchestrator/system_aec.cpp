#include "orchestrator/system_aec.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace acva::orchestrator {

namespace detail {

namespace {

// Pull the value of `key=` (e.g. "source_name=") out of a
// space/tab-separated args fragment. Returns empty if absent or empty.
std::string extract_kv(std::string_view args, std::string_view key) {
    auto pos = args.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    const auto end = args.find_first_of(" \t\n", pos);
    const auto len = (end == std::string_view::npos)
                         ? std::string_view::npos
                         : end - pos;
    return std::string(args.substr(pos, len));
}

} // namespace

std::optional<EchoCancelModule>
parse_echo_cancel_module_line(std::string_view line) {
    static constexpr std::string_view kName = "module-echo-cancel";
    const auto name_pos = line.find(kName);
    if (name_pos == std::string_view::npos) return std::nullopt;

    // pactl list short emits tab-separated columns: id \t name \t args.
    // The id is the prefix up to the first whitespace.
    const auto first_ws = line.find_first_of(" \t");
    if (first_ws == std::string_view::npos || first_ws == 0)
        return std::nullopt;

    EchoCancelModule m;
    m.id = std::string(line.substr(0, first_ws));
    for (char c : m.id) {
        if (c < '0' || c > '9') return std::nullopt;
    }

    // The args column starts at the first whitespace AFTER the module
    // name. If the line is malformed (no args column), source/sink
    // simply stay empty.
    const auto args_start = line.find_first_of(" \t", name_pos + kName.size());
    const auto args = (args_start == std::string_view::npos)
                          ? std::string_view{}
                          : line.substr(args_start);
    m.source_name = extract_kv(args, "source_name=");
    m.sink_name   = extract_kv(args, "sink_name=");
    return m;
}

} // namespace detail

namespace {

// popen + fully drain stdout. Sets `ok` to true iff exit status was 0.
std::string popen_capture(const std::string& cmd, bool& ok) {
    ok = false;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    std::array<char, 4096> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
        out.append(buf.data(), n);
    }
    const int rc = ::pclose(pipe);
    ok = (rc == 0);
    return out;
}

std::string trim(std::string s) {
    auto sp = [](char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; };
    while (!s.empty() && sp(s.front())) s.erase(s.begin());
    while (!s.empty() && sp(s.back()))  s.pop_back();
    return s;
}

// Walk `pactl list short modules` for the first module-echo-cancel
// entry and parse it. Returns nullopt iff pactl failed or no such
// module is loaded.
std::optional<detail::EchoCancelModule> find_existing_echo_cancel() {
    bool ok = false;
    const auto out = popen_capture(
        "pactl list short modules 2>/dev/null", ok);
    if (!ok) return std::nullopt;
    std::string line;
    auto try_match = [](const std::string& l)
        -> std::optional<detail::EchoCancelModule> {
        return detail::parse_echo_cancel_module_line(l);
    };
    for (char c : out) {
        if (c == '\n') {
            if (auto m = try_match(line)) return m;
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (auto m = try_match(line)) return m; // tail without trailing \n
    return std::nullopt;
}

// Load module-echo-cancel with names + args we control, so the
// destructor can unload precisely the module we created and the
// env vars set up next match. Returns the new module id or "" on
// failure.
//
// Args mirror `plans/milestones/m6b_aec_hardware.md` § 3.1: webrtc
// backend, NS + HPF on, AGC analog off (PipeWire's analog AGC
// fights with our APM AGC; better off here too).
std::string load_module() {
    bool ok = false;
    static constexpr const char* kCmd =
        "pactl load-module module-echo-cancel "
        "source_name=acva-echo-source sink_name=acva-echo-sink "
        "aec_method=webrtc "
        "aec_args=\""
            "analog_gain_control=0 digital_gain_control=1 "
            "noise_suppression=1 high_pass_filter=1\" "
        "2>/dev/null";
    auto out = trim(popen_capture(kCmd, ok));
    if (!ok || out.empty()) return {};
    // pactl returns just the decimal id on success. Sanity-check.
    for (char c : out) {
        if (c < '0' || c > '9') return {};
    }
    return out;
}

bool unload_module(std::string_view id) {
    if (id.empty()) return true;
    bool ok = false;
    (void)popen_capture(
        fmt::format("pactl unload-module {} 2>/dev/null", id), ok);
    return ok;
}

} // namespace

SystemAec::SystemAec(const config::ApmConfig& apm) {
    if (!apm.use_system_aec) return;

    static constexpr std::string_view kAcvaSource = "acva-echo-source";
    static constexpr std::string_view kAcvaSink   = "acva-echo-sink";

    if (auto existing = find_existing_echo_cancel()) {
        if (existing->source_name.empty() || existing->sink_name.empty()) {
            // Module is loaded but its args don't expose source_name=
            // and/or sink_name=. We can't reliably guess what PA-side
            // node names PipeWire assigned, and getting it wrong makes
            // PortAudio fall back to the system default — silently
            // routing audio AROUND the AEC. That was the M6B Step 4.2
            // false-pass mode (2026-05-04, soak gate 1 reported 33/min
            // false starts because the env vars pointed at non-existent
            // PA nodes). Refuse to start instead.
            startup_error_ = fmt::format(
                "module-echo-cancel id={} is loaded but its arguments "
                "lack source_name= and/or sink_name= "
                "(see `pactl list short modules`). acva can't "
                "reliably route audio through it. Either unload it "
                "(`pactl unload-module {}`) and let acva load its own, "
                "or set cfg.apm.use_system_aec: false.",
                existing->id, existing->id);
            log::error("system_aec", *startup_error_);
            return;
        }

        module_id_   = existing->id;
        source_name_ = existing->source_name;
        sink_name_   = existing->sink_name;
        // Adopt ownership iff the names match our convention. That
        // means the module was almost certainly loaded by a prior
        // acva run that didn't get to clean up (crash, kill -9, ...).
        // Adopting it lets the destructor unload on this run's clean
        // exit, preventing sticky bad state across crashes.
        owned_ = (source_name_ == kAcvaSource &&
                  sink_name_   == kAcvaSink);
        log::info("system_aec",
            fmt::format("module-echo-cancel already loaded (id={}); {}. "
                        "Routing this process via PULSE_SINK={}, "
                        "PULSE_SOURCE={}.",
                        existing->id,
                        owned_ ? "names match acva convention — "
                                  "adopting ownership and will unload on exit"
                                : "reusing without ownership",
                        sink_name_, source_name_));
    } else {
        const auto id = load_module();
        if (id.empty()) {
            startup_error_ =
                "use_system_aec=true but `pactl load-module "
                "module-echo-cancel` failed. Is pactl installed and "
                "PulseAudio/PipeWire running? Set "
                "cfg.apm.use_system_aec: false to disable system AEC "
                "(note: the in-process APM path is known to be "
                "ineffective on the dev workstation's laptop codec — "
                "see docs/aec_report.md).";
            log::error("system_aec", *startup_error_);
            return;
        }
        module_id_   = id;
        owned_       = true;
        source_name_ = std::string(kAcvaSource);
        sink_name_   = std::string(kAcvaSink);
        log::info("system_aec",
            fmt::format("loaded module-echo-cancel (id={}); routing "
                          "this process via PULSE_SINK={}, "
                          "PULSE_SOURCE={}; will unload on shutdown",
                          id, sink_name_, source_name_));
    }

    ::setenv("PULSE_SINK",   sink_name_.c_str(),   /*overwrite=*/1);
    ::setenv("PULSE_SOURCE", source_name_.c_str(), /*overwrite=*/1);
    active_ = true;
}

SystemAec::~SystemAec() {
    if (!owned_) return;
    if (unload_module(module_id_)) {
        log::info("system_aec",
            fmt::format("unloaded module-echo-cancel (id={})",
                          module_id_));
    } else {
        log::warn("system_aec",
            fmt::format("failed to unload module-echo-cancel (id={}) "
                          "— run `pactl unload-module {}` to clean up",
                          module_id_, module_id_));
    }
}

} // namespace acva::orchestrator
