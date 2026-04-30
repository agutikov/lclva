#include "config/config.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "http/server.hpp"
#include "log/log.hpp"
#include "memory/memory_thread.hpp"
#include "memory/recovery.hpp"
#include "metrics/registry.hpp"
#include "pipeline/fake_driver.hpp"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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
    std::signal(SIGHUP, handle_signal);
}

struct CliArgs {
    std::filesystem::path config_path = "config/default.yaml";
    bool show_help = false;
    bool no_fake_driver = false; // override config to disable
};

void print_help() {
    std::cout << "lclva — Long Conversation Local Voice Agent\n"
                 "\n"
                 "Usage: lclva [--config PATH] [--no-fake-driver]\n"
                 "\n"
                 "Options:\n"
                 "  --config PATH        YAML config file (default: config/default.yaml)\n"
                 "  --no-fake-driver     Disable the synthetic pipeline driver (M0)\n"
                 "  -h, --help           Show this help and exit\n";
}

CliArgs parse_args(int argc, char** argv) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            args.show_help = true;
        } else if (a == "--no-fake-driver") {
            args.no_fake_driver = true;
        } else if (a == "--config" && i + 1 < argc) {
            args.config_path = argv[++i];
        } else if (a.starts_with("--config=")) {
            args.config_path = a.substr(std::string_view{"--config="}.size());
        } else {
            std::cerr << "lclva: unknown argument: " << a << "\n";
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

    // ----- 1. Load config -----
    auto load_result = lclva::config::load_from_file(args.config_path);
    if (auto* err = std::get_if<lclva::config::LoadError>(&load_result)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto cfg = std::get<lclva::config::Config>(std::move(load_result));

    if (args.no_fake_driver) {
        cfg.pipeline.fake_driver_enabled = false;
    }

    // ----- 2. Initialize logging -----
    lclva::log::init(cfg.logging);
    lclva::log::info("main", "lclva starting");
    lclva::log::info("main", fmt::format("config loaded: {}", args.config_path.string()));

    install_signal_handlers();

    // ----- 3. Build the runtime -----
    lclva::event::EventBus bus;
    auto registry = std::make_shared<lclva::metrics::Registry>();
    auto metric_subs = registry->subscribe(bus); // keep-alive

    // Memory layer: open the database (or create it) and run the recovery
    // sweep so any in-flight turns from a previous crash get cleaned up
    // before we accept new traffic.
    auto mem_or = lclva::memory::MemoryThread::open(
        cfg.memory.db_path, cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<lclva::memory::DbError>(&mem_or)) {
        std::cerr << "memory: " << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<lclva::memory::MemoryThread>>(mem_or));

    {
        auto sweep = memory->read([](lclva::memory::Repository& repo) {
            return lclva::memory::run_recovery(repo, repo.database());
        });
        if (auto* err = std::get_if<lclva::memory::DbError>(&sweep)) {
            lclva::log::error("main",
                fmt::format("recovery sweep failed: {}", err->message));
            return EXIT_FAILURE;
        }
        const auto s = std::get<lclva::memory::RecoverySummary>(sweep);
        lclva::log::info("main", fmt::format(
            "recovery: closed {} sessions, marked {} turns interrupted, "
            "{} stale of {} summaries",
            s.sessions_closed, s.turns_marked_interrupted,
            s.summaries_stale, s.summaries_total));
    }

    lclva::dialogue::TurnFactory turns;
    lclva::dialogue::Fsm fsm(bus, turns);
    fsm.set_turn_outcome_observer([registry](const char* outcome) {
        registry->on_turn_outcome(outcome);
    });
    fsm.start();

    // FSM-state metric updater.
    lclva::event::SubscribeOptions fsm_metric_opts;
    fsm_metric_opts.name = "metrics.fsm_state";
    fsm_metric_opts.queue_capacity = 256;
    fsm_metric_opts.policy = lclva::event::OverflowPolicy::DropOldest;
    auto fsm_metric_sub = bus.subscribe_all(fsm_metric_opts,
        [registry, &fsm](const lclva::event::Event& /*e*/) {
            const auto snap = fsm.snapshot();
            registry->set_fsm_state(std::string(lclva::dialogue::to_string(snap.state)).c_str());
        });
    metric_subs.push_back(fsm_metric_sub);

    // HTTP control plane (/metrics, /status, /health).
    std::unique_ptr<lclva::http::ControlServer> control;
    try {
        control = std::make_unique<lclva::http::ControlServer>(cfg.control, registry, &fsm);
    } catch (const std::exception& ex) {
        lclva::log::error("main", fmt::format("control server failed to start: {}", ex.what()));
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }

    // Optional fake pipeline driver.
    std::unique_ptr<lclva::pipeline::FakeDriver> fake_driver;
    if (cfg.pipeline.fake_driver_enabled) {
        lclva::pipeline::FakeDriverOptions opts;
        opts.sentences_per_turn = cfg.pipeline.fake_sentences_per_turn;
        opts.idle_between_turns = std::chrono::milliseconds{cfg.pipeline.fake_idle_between_turns_ms};
        opts.barge_in_probability = cfg.pipeline.fake_barge_in_probability;
        fake_driver = std::make_unique<lclva::pipeline::FakeDriver>(bus, opts);
        fake_driver->start();
        lclva::log::info("main", "fake pipeline driver enabled");
    } else {
        lclva::log::info("main", "fake pipeline driver disabled");
    }

    // ----- 4. Main loop -----
    while (g_signal_received.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int sig = g_signal_received.load();
    lclva::log::info("main", fmt::format("received signal {}, shutting down", sig));

    // Orderly shutdown: stop producers first, then drain.
    if (fake_driver) {
        fake_driver->stop();
    }
    fsm.stop();
    control.reset();
    bus.shutdown();

    lclva::log::info("main", "lclva exited cleanly");
    return EXIT_SUCCESS;
}
