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
        } else if (a == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (a.starts_with("--config=")) {
            args.config_path = a.substr(std::string_view{"--config="}.size());
        } else if (a == "demo") {
            // `demo` subcommand: optional positional <name>. No name →
            // print the catalog.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                args.demo = argv[++i];
            } else {
                args.demo = "list";
            }
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
                 "  -h, --help           Show this help and exit\n"
                 "\n"
                 "Subcommands:\n"
                 "  demo                 List per-milestone demo commands.\n"
                 "  demo <name>          Run a demo end-to-end (no user input). E.g.\n"
                 "                       `acva demo tone` plays a 1.5 s sine wave to\n"
                 "                       verify the audio device. See `acva demo` for\n"
                 "                       the catalog.\n"
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
