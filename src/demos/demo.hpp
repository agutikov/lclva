#pragma once

#include "config/config.hpp"

#include <span>
#include <string_view>

namespace acva::demos {

// One self-contained "does milestone N's deliverable work?" check that
// the user can run without typing anything. Each demo:
//
//   • Builds only the subsystems it needs from the loaded Config.
//   • Talks to its target backend(s) — does NOT mock anything. The
//     point is to verify the wiring on the user's machine.
//   • Returns 0 on success, non-zero on observable failure.
//   • Exits within a few seconds. Long-running demos should expose
//     their own timeout knob; the framework doesn't enforce one.
//
// Demos are intentionally separate from the test suite: they need
// real backends (Piper, llama, an audio device), so they live in the
// shipped binary rather than in `acva_tests`.
struct Demo {
    std::string_view name;          // e.g. "tone"; argv shorthand
    std::string_view milestone;     // "M0" / "M1" / ... — for help output
    std::string_view description;   // one-line summary

    // Returns 0 on success, non-zero on failure. May write progress
    // lines to stdout and diagnostic JSON-lines to stderr (via the
    // shared logger).
    int (*run)(const config::Config& cfg) = nullptr;
};

// All demos compiled into the binary, in the order they should be
// listed by `acva demo`.
[[nodiscard]] std::span<const Demo> all();

// Lookup by exact name. Returns nullptr if unknown.
[[nodiscard]] const Demo* find(std::string_view name);

// Print the catalog to stdout in a form suitable for `acva demo`
// with no argument. Each row: "  <name>  <milestone>  <description>".
void print_list();

} // namespace acva::demos
