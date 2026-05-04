#include "cli/args.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace acva::cli {

namespace {

std::atomic<int> g_signal_received{0}; // NOLINT(*global*)

void handle_signal(int sig) {
    g_signal_received.store(sig);
}

} // namespace

CliArgs parse_args(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            args.show_help = true;
        } else if (a == "--stdin") {
            args.stdin_mode = true;
        } else if (a == "--stdin-lang" && i + 1 < argc) {
            args.stdin_lang = argv[++i];
        } else if (a.starts_with("--stdin-lang=")) {
            args.stdin_lang = a.substr(std::string_view{"--stdin-lang="}.size());
        } else if (a == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (a.starts_with("--config=")) {
            args.config_path = a.substr(std::string_view{"--config="}.size());
        } else if (a == "demo") {
            // `demo` subcommand: optional positional <name>, then
            // anything-goes trailing args that get passed through to
            // the demo's run() function. The orchestrator itself does
            // not validate trailing args — the demo decides what's
            // legal. Stop the orchestrator's own parsing here so flags
            // like `--delay-ms` aren't rejected by the unknown-arg
            // branch below.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                args.demo = argv[++i];
            } else {
                args.demo = "list";
            }
            for (int j = i + 1; j < argc; ++j) {
                args.demo_args.emplace_back(argv[j]);
            }
            break;
        } else {
            std::cerr << "acva: unknown argument: " << a << "\n";
            std::exit(EXIT_FAILURE);
        }
    }
    return args;
}

void print_help() {
    std::cout << "acva — Autonomous Conversational Voice Agent\n"
                 "\n"
                 "Usage: acva [--config PATH] [--stdin]\n"
                 "       acva [--config PATH] demo [<name>]\n"
                 "\n"
                 "Options:\n"
                 "  --config PATH        YAML config file. If omitted, search:\n"
                 "                         ${XDG_CONFIG_HOME:-~/.config}/acva/default.yaml,\n"
                 "                         ./config/default.yaml,\n"
                 "                         /etc/acva/default.yaml\n"
                 "                       (first existing path wins).\n"
                 "  --stdin              Read FinalTranscript lines from stdin and drive\n"
                 "                       the real LLM. M1 mode.\n"
                 "  --stdin-lang LANG    Stamp this lang on every --stdin line\n"
                 "                       (overrides cfg.dialogue.fallback_language).\n"
                 "                       Routes the LLM to the matching system_prompt\n"
                 "                       and TTS to the matching voice. E.g. `ru`.\n"
                 "  -h, --help           Show this help and exit\n"
                 "\n"
                 "Subcommands:\n"
                 "  demo                 List per-milestone demo commands.\n"
                 "  demo <name>          Run a demo end-to-end (no user input). E.g.\n"
                 "                       `acva demo tone` plays a 1.5 s sine wave to\n"
                 "                       verify the audio device. See `acva demo` for\n"
                 "                       the catalog. Trailing args after the demo\n"
                 "                       name are passed through to the demo (e.g.\n"
                 "                       `acva demo bargein --delay-ms 800`).\n"
                 "\n"
                 "To exercise the FSM without backends, set\n"
                 "`pipeline.fake_driver_enabled: true` in the YAML.\n";
}

void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, handle_signal);
}

int signal_received() noexcept {
    return g_signal_received.load(std::memory_order_relaxed);
}

void request_shutdown(int sig) noexcept {
    g_signal_received.store(sig);
}

} // namespace acva::cli
