#include "config/config.hpp"
#include "log/log.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

namespace {

std::atomic<int> g_signal_received{0};

void handle_signal(int sig) {
    g_signal_received.store(sig);
}

void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, handle_signal); // reserved for hot reload later
}

struct CliArgs {
    std::filesystem::path config_path = "config/default.yaml";
    bool show_help = false;
};

void print_help() {
    std::cout << "lclva — Long Conversation Local Voice Agent\n"
                 "\n"
                 "Usage: lclva [--config PATH]\n"
                 "\n"
                 "Options:\n"
                 "  --config PATH   YAML config file (default: config/default.yaml)\n"
                 "  -h, --help      Show this help and exit\n";
}

CliArgs parse_args(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            args.show_help = true;
        } else if (a == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (a.starts_with("--config=")) {
            args.config_path = a.substr(std::string_view{"--config="}.size());
        } else {
            std::cerr << "lclva: unknown argument: " << a << "\n";
            args.show_help = true;
            std::exit(EXIT_FAILURE);
        }
    }
    return args;
}

} // namespace

int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);
    if (args.show_help) {
        print_help();
        return EXIT_SUCCESS;
    }

    auto load_result = lclva::config::load_from_file(args.config_path);
    if (auto* err = std::get_if<lclva::config::LoadError>(&load_result)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto cfg = std::get<lclva::config::Config>(std::move(load_result));

    lclva::log::init(cfg.logging);
    lclva::log::info("main", "lclva starting");
    lclva::log::info("main", std::string{"config loaded: "} + args.config_path.string());

    install_signal_handlers();

    // M0 placeholder: orchestrator main loop. Real pipeline wires in M1+.
    // For now, just block on signal so users can verify clean startup/shutdown.
    while (g_signal_received.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int sig = g_signal_received.load();
    lclva::log::info("main", std::string{"received signal "} + std::to_string(sig) + ", shutting down");
    lclva::log::info("main", "lclva exited cleanly");
    return EXIT_SUCCESS;
}
