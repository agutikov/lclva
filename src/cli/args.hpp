#pragma once

#include <filesystem>
#include <string>

namespace acva::cli {

// Parsed `acva` command-line arguments.
struct CliArgs {
    // Empty → resolve via config::resolve_config_path (XDG_CONFIG_HOME first,
    // then ./config/default.yaml, then /etc/acva/default.yaml).
    std::filesystem::path config_path;
    bool show_help = false;
    bool stdin_mode = false;     // M1: read FinalTranscript lines from stdin

    // Override the lang stamped on FinalTranscripts published from
    // --stdin. Empty → use cfg.dialogue.fallback_language. Affects
    // PromptBuilder system_prompt selection and TTS voice routing
    // for the synthetic stdin path only; the real STT path resolves
    // language from Whisper.
    std::string stdin_lang;

    // Per-milestone smoke-check subcommand. Empty → run normally.
    // "list" → print the catalog and exit. Otherwise, look up by name
    // in acva::demos::find().
    std::string demo;
};

// Parse argv. Exits the process with EXIT_FAILURE on unknown options
// (the orchestrator can't continue past a typo) — same behaviour as
// the previous in-main implementation.
CliArgs parse_args(int argc, char** argv);

// Print the long help text to stdout. Caller usually checks
// args.show_help and returns EXIT_SUCCESS after.
void print_help();

// SIGINT/SIGTERM/SIGHUP handler installer. Sets the global counter
// returned by signal_received() so the main loop can drain cleanly.
void install_signal_handlers();

// Most-recent signal number, or 0 if none received yet. Read from any
// thread.
[[nodiscard]] int signal_received() noexcept;

// Mark a synthetic shutdown request — the stdin EOF path uses this
// to wake the main loop without a real signal. Argument is the
// notional signal number (typically SIGTERM).
void request_shutdown(int sig) noexcept;

} // namespace acva::cli
