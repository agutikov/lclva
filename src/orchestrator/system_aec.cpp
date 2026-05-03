#include "orchestrator/system_aec.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace acva::orchestrator {

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

// Walk `pactl list short modules` for a module-echo-cancel entry.
// Returns its id (decimal string) if found, empty otherwise.
std::string find_existing_echo_cancel() {
    bool ok = false;
    const auto out = popen_capture(
        "pactl list short modules 2>/dev/null", ok);
    if (!ok) return {};
    std::string line;
    for (char c : out) {
        if (c == '\n') {
            if (line.find("module-echo-cancel") != std::string::npos) {
                const auto ws = line.find_first_of(" \t");
                if (ws != std::string::npos) {
                    return std::string(line.substr(0, ws));
                }
            }
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    return {};
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

    const auto existing = find_existing_echo_cancel();
    if (!existing.empty()) {
        // Reuse — do not take ownership.  We can't tell what
        // source_name / sink_name the existing module was loaded
        // with, so we use the PipeWire default names (which are
        // what `pactl load-module module-echo-cancel` produces with
        // no args).  If the user loaded it with custom names, the
        // env vars below won't match and PortAudio will fall back
        // to the system default — log warns so they can either
        // unload + relaunch or accept the default routing.
        module_id_   = existing;
        owned_       = false;
        source_name_ = "echo-cancel-source";
        sink_name_   = "echo-cancel-sink";
        log::info("system_aec",
            fmt::format("module-echo-cancel already loaded (id={}); "
                          "reusing without ownership.  Routing this "
                          "process via PULSE_SINK={}, PULSE_SOURCE={}.  "
                          "If those names don't match the existing "
                          "module's source/sink, audio will fall back "
                          "to the system default.",
                          existing, sink_name_, source_name_));
    } else {
        const auto id = load_module();
        if (id.empty()) {
            log::warn("system_aec",
                "use_system_aec=true but `pactl load-module "
                "module-echo-cancel` failed.  Is pactl installed "
                "and PulseAudio/PipeWire running?  Falling back to "
                "the default audio path.");
            return;
        }
        module_id_   = id;
        owned_       = true;
        source_name_ = "acva-echo-source";
        sink_name_   = "acva-echo-sink";
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
