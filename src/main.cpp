#include "cli/args.hpp"
#include "config/config.hpp"
#include "demos/demo.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "http/server.hpp"
#include "log/log.hpp"
#include "memory/memory_thread.hpp"
#include "memory/recovery.hpp"
#include "memory/repository.hpp"
#include "metrics/registry.hpp"
#include "orchestrator/bootstrap.hpp"
#include "orchestrator/capture_stack.hpp"
#include "orchestrator/dialogue_stack.hpp"
#include "orchestrator/event_tracer.hpp"
#include "orchestrator/status_extra.hpp"
#include "orchestrator/stt_stack.hpp"
#include "orchestrator/supervisor_setup.hpp"
#include "orchestrator/system_aec.hpp"
#include "orchestrator/tts_stack.hpp"
#include "stt/openai_stt_client.hpp"
#include "supervisor/supervisor.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <variant>

int main(int argc, char** argv) {
    auto args = acva::cli::parse_args(argc, argv);
    if (args.show_help) {
        acva::cli::print_help();
        return EXIT_SUCCESS;
    }

    // ----- 1. Load + resolve config -----
    auto loaded = acva::orchestrator::load_and_resolve_config(
        args.config_path, args.stdin_mode);
    if (auto* err = std::get_if<acva::config::LoadError>(&loaded)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto& bundle      = std::get<acva::orchestrator::LoadedConfig>(loaded);
    auto& cfg         = bundle.cfg;
    const auto config_path = bundle.config_path;

    // ----- 2. Initialize logging + ALSA sidestep -----
    acva::log::init(cfg.logging);
    acva::log::info("main", "acva starting");
    acva::log::info("main", fmt::format("config loaded: {}", config_path.string()));
    acva::log::info("main", fmt::format("memory db: {}", cfg.memory.db_path));
    acva::orchestrator::install_alsa_sidestep(cfg.audio);

    // Validate --stdin-lang against the configured maps.  Empty maps
    // mean the feature is not configured (early-bring-up state) — only
    // gate when a map has entries at all, mirroring config::validate.
    if (args.stdin_mode && !args.stdin_lang.empty()) {
        std::vector<std::string> errs;
        if (!cfg.dialogue.system_prompts.empty()
            && !cfg.dialogue.system_prompts.contains(args.stdin_lang)) {
            errs.push_back(fmt::format(
                "dialogue.system_prompts has no entry for '{}'",
                args.stdin_lang));
        }
        if (!cfg.tts.voices.empty()
            && !cfg.tts.voices.contains(args.stdin_lang)) {
            errs.push_back(fmt::format(
                "tts.voices has no entry for '{}'", args.stdin_lang));
        }
        if (!errs.empty()) {
            std::cerr << "acva: --stdin-lang " << args.stdin_lang
                      << " is not configured:\n";
            for (const auto& e : errs) std::cerr << "  - " << e << "\n";
            std::cerr << "Add the missing entries to "
                      << config_path.string()
                      << " or pick a configured lang.\n";
            return EXIT_FAILURE;
        }
    }

    // RAII: optionally load PipeWire's module-echo-cancel and route
    // this process through it (set PULSE_SINK / PULSE_SOURCE before
    // any PortAudio init).  No-op when cfg.apm.use_system_aec=false.
    // Lives at function scope so its destructor unloads the module on
    // exit (including the demo short-circuit below).
    acva::orchestrator::SystemAec system_aec(cfg.apm);
    if (const auto& err = system_aec.startup_error()) {
        // System-AEC setup failed in a way that would cause silent
        // misrouting. Refuse to start rather than fall back — see the
        // failure-modes block in system_aec.hpp.
        std::cerr << "acva: system AEC setup failed: " << *err << "\n";
        return EXIT_FAILURE;
    }

    // ----- 2.5. Demo subcommand short-circuit -----
    // Demos build only the subsystems they need from `cfg` and exit
    // when done. They MUST run before the full runtime (memory thread,
    // fsm, supervisor, ...) is constructed — otherwise we'd be paying
    // SQLite + bus + threads for a fast smoke check.
    if (!args.demo.empty()) {
        if (args.demo == "list") {
            acva::demos::print_list();
            return EXIT_SUCCESS;
        }
        const auto* d = acva::demos::find(args.demo);
        if (!d) {
            std::cerr << "acva: unknown demo '" << args.demo
                       << "' — try `acva demo` for the list.\n";
            return EXIT_FAILURE;
        }
        return d->run(cfg);
    }

    acva::cli::install_signal_handlers();

    // Production-only periodic VRAM probe. Spawned AFTER the demo
    // dispatch above — demos do their own VRAM probing and a joinable
    // thread destructor on demo-return would call std::terminate.
    // RAII: stops + joins on destruction at end of main.
    acva::orchestrator::VramMonitor vram_monitor(cfg.logging);

    // ----- 3. Build the runtime -----
    acva::event::EventBus bus;
    auto registry = std::make_shared<acva::metrics::Registry>();
    auto metric_subs = registry->subscribe(bus); // keep-alive

    // Memory layer: open the database (or create it) and run the recovery
    // sweep so any in-flight turns from a previous crash get cleaned up
    // before we accept new traffic.
    auto mem_or = acva::memory::MemoryThread::open(
        cfg.memory.db_path, cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<acva::memory::DbError>(&mem_or)) {
        std::cerr << "memory: " << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<acva::memory::MemoryThread>>(mem_or));

    {
        auto sweep = memory->read([](acva::memory::Repository& repo) {
            return acva::memory::run_recovery(repo, repo.database());
        });
        if (auto* err = std::get_if<acva::memory::DbError>(&sweep)) {
            acva::log::error("main",
                fmt::format("recovery sweep failed: {}", err->message));
            return EXIT_FAILURE;
        }
        const auto s = std::get<acva::memory::RecoverySummary>(sweep);
        acva::log::info("main", fmt::format(
            "recovery: closed {} sessions, marked {} turns interrupted, "
            "{} stale of {} summaries",
            s.sessions_closed, s.turns_marked_interrupted,
            s.summaries_stale, s.summaries_total));
    }

    acva::dialogue::TurnFactory turns;
    acva::dialogue::Fsm fsm(bus, turns);
    fsm.set_turn_outcome_observer([registry](const char* outcome) {
        registry->on_turn_outcome(outcome);
    });
    fsm.start();

    // FSM-state metric updater.
    acva::event::SubscribeOptions fsm_metric_opts;
    fsm_metric_opts.name = "metrics.fsm_state";
    fsm_metric_opts.queue_capacity = 256;
    fsm_metric_opts.policy = acva::event::OverflowPolicy::DropOldest;
    auto fsm_metric_sub = bus.subscribe_all(fsm_metric_opts,
        [registry, &fsm](const acva::event::Event& /*e*/) {
            const auto snap = fsm.snapshot();
            registry->set_fsm_state(std::string(acva::dialogue::to_string(snap.state)).c_str());
        });
    metric_subs.push_back(fsm_metric_sub);

    // ----- Event tracer (cfg.logging.trace_events) -----
    if (auto sub = acva::orchestrator::install_event_tracer(bus, cfg.logging)) {
        metric_subs.push_back(std::move(sub));
    }

    // ----- M2: Supervisor -----
    auto supervisor_ptr = acva::orchestrator::build_supervisor(
        cfg, bus, registry, metric_subs);
    auto& supervisor = *supervisor_ptr;

    // ----- M3: TTS + playback stack -----
    //
    // Track the turn id the Manager mints for each LLM run. The
    // PlaybackEngine reads this to filter "live" chunks. Manager
    // updates it synchronously via its turn-started hook (set in
    // the dialogue stack below) BEFORE LlmStarted publishes — the
    // earlier async bus subscriber was racy and stranded the FSM
    // in Speaking when TTS chunks raced the LlmStarted subscriber.
    auto playback_active_turn =
        std::make_shared<std::atomic<acva::event::TurnId>>(acva::event::kNoTurn);

    auto tts = acva::orchestrator::build_tts_stack(
        cfg, bus, registry, playback_active_turn);
    auto* loopback_sink   = tts->loopback();

    // Forward-declared so the /status closure can read APM stats at
    // request time. The capture stack is actually built below (M4
    // section) — we declare the unique_ptr early and the lambda picks
    // up its value lazily on every /status hit.
    std::unique_ptr<acva::orchestrator::CaptureStack> capture;

    // HTTP control plane (/metrics, /status, /health).
    std::unique_ptr<acva::http::ControlServer> control;
    try {
        control = std::make_unique<acva::http::ControlServer>(
            cfg.control, registry, &fsm,
            acva::orchestrator::make_status_extra(supervisor, capture));
    } catch (const std::exception& ex) {
        acva::log::error("main", fmt::format("control server failed to start: {}", ex.what()));
        supervisor.stop();
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }

    // ----- M4 + M6: capture + VAD + APM pipeline -----
    // STT warm-up sits between TTS and capture: it loads the Whisper
    // model into VRAM before the mic opens so the first user turn
    // doesn't pay the model-load cost. Best-effort — failure logs
    // a warning and continues.
    if (cfg.audio.capture_enabled
        && cfg.stt.warmup_on_startup
        && !cfg.stt.base_url.empty()) {
        acva::log::info("main", fmt::format(
            "warming up STT (model={}) — blocking until ready…",
            cfg.stt.model));
        const auto r = acva::stt::warmup(cfg.stt);
        if (r.ok) {
            acva::log::info("main", fmt::format(
                "STT warm-up complete in {} ms", r.ms));
        } else {
            acva::log::warn("main", fmt::format(
                "STT warm-up failed in {} ms ({}); first user turn "
                "will pay the model-load cost", r.ms, r.error));
        }
    }
    capture = acva::orchestrator::build_capture_stack(
        cfg, bus, registry, fsm, loopback_sink);
    auto* audio_pipeline = capture->pipeline();

    // ----- M4B + M5 STT path -----
    auto stt_stack = acva::orchestrator::build_stt_stack(
        cfg, bus, audio_pipeline, metric_subs);

    // ----- M1 + M2 + M5 dialogue + LLM stack -----
    auto dialogue_or = acva::orchestrator::build_dialogue_stack(
        cfg, bus, registry, *memory, fsm, supervisor, turns,
        playback_active_turn, metric_subs);
    if (auto* err = std::get_if<acva::memory::DbError>(&dialogue_or)) {
        acva::log::error("main", fmt::format("session insert failed: {}", err->message));
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }
    auto dialogue = std::move(std::get<std::unique_ptr<acva::orchestrator::DialogueStack>>(dialogue_or));
    std::thread stdin_reader;

    // ----- stdin text-input mode (M1 era) — only the line reader -----
    if (args.stdin_mode) {
        const std::string stdin_lang = args.stdin_lang.empty()
                                           ? cfg.dialogue.fallback_language
                                           : args.stdin_lang;
        std::cout << "acva stdin mode (lang=" << stdin_lang
                  << ") — type a line and press enter. Ctrl-D or Ctrl-C to exit.\n";
        stdin_reader = std::thread([&bus, lang = stdin_lang]{
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty()) continue;
                bus.publish(acva::event::FinalTranscript{
                    .turn = 0,
                    .text = line,
                    .lang = lang,
                    .confidence = 1.0F,
                    .audio_duration = {},
                    .processing_duration = {},
                });
            }
            // EOF on stdin (Ctrl-D or stdin closed) — request shutdown.
            acva::cli::request_shutdown(SIGTERM);
        });
    }

    // ----- 4. Main loop -----
    while (acva::cli::signal_received() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int sig = acva::cli::signal_received();
    acva::log::info("main", fmt::format("received signal {}, shutting down", sig));

    // Wake the stdin reader if it's still in getline().
    if (stdin_reader.joinable()) {
        ::close(STDIN_FILENO);
        stdin_reader.join();
    }

    // Orderly shutdown: stop producers first, then drain.
    // Each stack's stop() is idempotent and respects internal
    // teardown ordering (capture before pipeline, bridge before
    // self-listen before engine, etc.).
    if (capture)  capture->stop();
    if (stt_stack) stt_stack->stop();
    if (dialogue) dialogue->stop();
    if (tts)      tts->stop();
    supervisor.stop();
    fsm.stop();
    control.reset();
    bus.shutdown();
    // VramMonitor stops in its destructor when main returns.

    acva::log::info("main", "acva exited cleanly");
    return EXIT_SUCCESS;
}
