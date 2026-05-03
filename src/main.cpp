#include "audio/capture.hpp"
#include "audio/clock.hpp"
#include "audio/loopback.hpp"
#include "audio/pipeline.hpp"
#include "config/config.hpp"
#include "config/paths.hpp"
#include "demos/demo.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/manager.hpp"
#include "dialogue/tts_bridge.hpp"
#include "dialogue/turn.hpp"
#include "dialogue/turn_writer.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "http/server.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "log/log.hpp"
#include "memory/memory_thread.hpp"
#include "memory/recovery.hpp"
#include "memory/repository.hpp"
#include "memory/summarizer.hpp"
#include "metrics/registry.hpp"
#include "pipeline/fake_driver.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "stt/openai_stt_client.hpp"
#include "stt/realtime_stt_client.hpp"
#include "supervisor/keep_alive.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"
#include "supervisor/supervisor.hpp"
#include "tts/openai_tts_client.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
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
    // Empty → resolve via config::resolve_config_path (XDG_CONFIG_HOME first,
    // then ./config/default.yaml, then /etc/acva/default.yaml).
    std::filesystem::path config_path;
    bool show_help = false;
    bool stdin_mode = false;     // M1: read FinalTranscript lines from stdin

    // Per-milestone smoke-check subcommand. Empty → run normally.
    // "list" → print the catalog and exit. Otherwise, look up by name
    // in acva::demos::find().
    std::string demo;
};

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

} // namespace

int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);
    if (args.show_help) {
        print_help();
        return EXIT_SUCCESS;
    }

    // ----- 1. Load config -----
    // Resolve --config first: empty → walk the XDG/in-tree/system search
    // list. An explicit --config is used verbatim and any "not found"
    // error comes from load_from_file.
    auto cp = acva::config::resolve_config_path(args.config_path.string());
    if (auto* err = std::get_if<acva::config::LoadError>(&cp)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    const auto config_path = std::get<std::filesystem::path>(std::move(cp));

    auto load_result = acva::config::load_from_file(config_path);
    if (auto* err = std::get_if<acva::config::LoadError>(&load_result)) {
        std::cerr << err->message << "\n";
        return EXIT_FAILURE;
    }
    auto cfg = std::get<acva::config::Config>(std::move(load_result));

    // --stdin still implies "fake driver off", since the user is the
    // event source. Without this the synthetic driver would race the
    // typed input.
    if (args.stdin_mode) {
        cfg.pipeline.fake_driver_enabled = false;
    }

    // Resolve the SQLite path: empty / relative → under XDG_DATA_HOME.
    // Mutates cfg in place so anything that re-reads cfg.memory.db_path
    // later (e.g. /status, log lines) sees the canonical absolute path.
    cfg.memory.db_path =
        acva::config::resolve_data_path(cfg.memory.db_path, "acva.db").string();

    // M4 — resolve the Silero VAD model path against XDG_DATA_HOME.
    // Empty → ${XDG_DATA_HOME}/acva/models/silero/silero_vad.onnx (the
    // path scripts/download-vad.sh writes to). If the file isn't
    // there, AudioPipeline catches the load failure and disables VAD
    // with a warning — so leaving model_path unset is safe.
    if (cfg.vad.model_path.empty()) {
        const auto resolved = acva::config::resolve_data_path(
            "", "models/silero/silero_vad.onnx");
        if (std::filesystem::exists(resolved)) {
            cfg.vad.model_path = resolved.string();
        }
    } else {
        cfg.vad.model_path =
            acva::config::resolve_data_path(cfg.vad.model_path, "silero/silero_vad.onnx").string();
    }

    // ----- 2. Initialize logging -----
    acva::log::init(cfg.logging);
    acva::log::info("main", "acva starting");
    acva::log::info("main", fmt::format("config loaded: {}", config_path.string()));
    acva::log::info("main", fmt::format("memory db: {}", cfg.memory.db_path));

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

    install_signal_handlers();

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
    //
    // One bus subscriber that prints a structured log line for each
    // interesting event in the dialogue/STT/LLM/TTS/playback path
    // with the relevant payload. Per-token / per-chunk events are
    // summarized rather than emitted individually. Useful for
    // end-to-end debugging the half-duplex speakers pipeline.
    if (cfg.logging.trace_events) {
        // Per-utterance counters that accumulate between LlmStarted
        // and LlmFinished / between PlaybackStarted and PlaybackFinished.
        // Captured by reference into the lambda; cleared on
        // start-of-stream events.
        struct StreamTallies {
            std::size_t llm_tokens   = 0;
            std::size_t tts_chunks   = 0;
            std::size_t tts_bytes    = 0;
        };
        auto tallies = std::make_shared<StreamTallies>();

        auto truncate = [](std::string_view s, std::size_t max = 200) {
            if (s.size() <= max) return std::string(s);
            std::string out(s.substr(0, max));
            out += "…";
            return out;
        };

        acva::event::SubscribeOptions trace_opts;
        trace_opts.name = "trace.events";
        trace_opts.queue_capacity = 1024;
        trace_opts.policy = acva::event::OverflowPolicy::DropOldest;
        auto trace_sub = bus.subscribe_all(trace_opts,
            [tallies, truncate](const acva::event::Event& e) {
                std::visit([&]<class T>(const T& ev) {
                    using namespace acva;
                    if constexpr (std::is_same_v<T, event::SpeechStarted>) {
                        log::event("trace", "speech_started", ev.turn, {});
                    } else if constexpr (std::is_same_v<T, event::SpeechEnded>) {
                        log::event("trace", "speech_ended", ev.turn, {});
                    } else if constexpr (std::is_same_v<T, event::PartialTranscript>) {
                        log::event("trace", "partial_transcript", ev.turn, {
                            {"seq",  std::to_string(ev.seq)},
                            {"lang", ev.lang},
                            {"text", truncate(ev.text)},
                        });
                    } else if constexpr (std::is_same_v<T, event::FinalTranscript>) {
                        log::event("trace", "final_transcript", ev.turn, {
                            {"lang",                  ev.lang},
                            {"audio_duration_ms",     std::to_string(ev.audio_duration.count())},
                            {"processing_duration_ms", std::to_string(ev.processing_duration.count())},
                            {"text",                  ev.text},
                        });
                    } else if constexpr (std::is_same_v<T, event::LlmStarted>) {
                        tallies->llm_tokens = 0;
                        log::event("trace", "llm_started", ev.turn, {});
                    } else if constexpr (std::is_same_v<T, event::LlmToken>) {
                        ++tallies->llm_tokens;
                    } else if constexpr (std::is_same_v<T, event::LlmSentence>) {
                        log::event("trace", "llm_sentence", ev.turn, {
                            {"seq",  std::to_string(ev.seq)},
                            {"lang", ev.lang},
                            {"text", ev.text},
                        });
                    } else if constexpr (std::is_same_v<T, event::LlmFinished>) {
                        log::event("trace", "llm_finished", ev.turn, {
                            {"tokens",    std::to_string(tallies->llm_tokens)},
                            {"cancelled", ev.cancelled ? "true" : "false"},
                        });
                    } else if constexpr (std::is_same_v<T, event::TtsStarted>) {
                        log::event("trace", "tts_started", ev.turn, {
                            {"seq", std::to_string(ev.seq)},
                        });
                    } else if constexpr (std::is_same_v<T, event::TtsAudioChunk>) {
                        tallies->tts_chunks += 1;
                        tallies->tts_bytes  += ev.bytes;
                    } else if constexpr (std::is_same_v<T, event::TtsFinished>) {
                        log::event("trace", "tts_finished", ev.turn, {
                            {"seq",    std::to_string(ev.seq)},
                            {"chunks", std::to_string(tallies->tts_chunks)},
                            {"bytes",  std::to_string(tallies->tts_bytes)},
                        });
                        tallies->tts_chunks = 0;
                        tallies->tts_bytes  = 0;
                    } else if constexpr (std::is_same_v<T, event::PlaybackStarted>) {
                        log::event("trace", "playback_started", ev.turn, {
                            {"seq", std::to_string(ev.seq)},
                        });
                    } else if constexpr (std::is_same_v<T, event::PlaybackFinished>) {
                        log::event("trace", "playback_finished", ev.turn, {
                            {"seq", std::to_string(ev.seq)},
                        });
                    } else if constexpr (std::is_same_v<T, event::UserInterrupted>) {
                        log::event("trace", "user_interrupted", ev.turn, {});
                    } else if constexpr (std::is_same_v<T, event::CancelGeneration>) {
                        log::event("trace", "cancel_generation", ev.turn, {});
                    }
                    // UtteranceReady / Pause / Resume / ErrorEvent /
                    // HealthChanged are handled elsewhere or have low
                    // signal value here.
                }, e);
            });
        metric_subs.push_back(trace_sub);
        acva::log::info("main", "event tracer enabled (cfg.logging.trace_events)");
    }

    // ----- M2: Supervisor -----
    //
    // One HttpProbe shared by every ServiceMonitor — it carries no
    // per-call state and uses the supervisor-wide probe_timeout_ms.
    // Wrapped in a shared_ptr so the ProbeFn closure keeps it alive
    // for the entire process lifetime.
    auto http_probe = std::make_shared<acva::supervisor::HttpProbe>(
        std::chrono::milliseconds(cfg.supervisor.probe_timeout_ms));
    acva::supervisor::ProbeFn probe_fn = [http_probe](std::string_view url) {
        return http_probe->get(url);
    };

    acva::supervisor::Supervisor supervisor(cfg.supervisor, bus, probe_fn);
    supervisor.register_service(
        acva::supervisor::ServiceConfig::from_health("llm", cfg.llm.health));
    supervisor.register_service(
        acva::supervisor::ServiceConfig::from_health("stt", cfg.stt.health));
    supervisor.register_service(
        acva::supervisor::ServiceConfig::from_health("tts", cfg.tts.health));

    // Pre-create the per-service health gauges so /metrics shows the
    // full grid before the first probe lands.
    if (!cfg.llm.health.health_url.empty()) registry->register_service_for_health("llm");
    if (!cfg.stt.health.health_url.empty()) registry->register_service_for_health("stt");
    if (!cfg.tts.health.health_url.empty()) registry->register_service_for_health("tts");

    // Mirror HealthChanged events into the metric.
    {
        acva::event::SubscribeOptions opts;
        opts.name = "metrics.health";
        opts.queue_capacity = 64;
        opts.policy = acva::event::OverflowPolicy::DropOldest;
        auto h = bus.subscribe<acva::event::HealthChanged>(opts,
            [registry](const acva::event::HealthChanged& e) {
                const char* s = "unknown";
                switch (e.state) {
                    case acva::event::HealthState::Unknown:   s = "unknown";   break;
                    case acva::event::HealthState::Healthy:   s = "healthy";   break;
                    case acva::event::HealthState::Degraded:  s = "degraded";  break;
                    case acva::event::HealthState::Unhealthy: s = "unhealthy"; break;
                }
                registry->set_health_state(e.service, s);
            });
        metric_subs.push_back(h);
    }

    // Pipeline-state metric updater. Polls supervisor.pipeline_state()
    // on every event so the gauge stays current; cheap because both
    // calls are atomic loads.
    {
        acva::event::SubscribeOptions opts;
        opts.name = "metrics.pipeline";
        opts.queue_capacity = 64;
        opts.policy = acva::event::OverflowPolicy::DropOldest;
        auto h = bus.subscribe_all(opts,
            [registry, &supervisor](const acva::event::Event&) {
                registry->set_pipeline_state(std::string(
                    acva::supervisor::to_string(supervisor.pipeline_state())).c_str());
            });
        metric_subs.push_back(h);
    }

    supervisor.start();

    // ----- M3: TTS + playback stack -----
    //
    // Constructed conditionally on cfg.tts.voices being non-empty —
    // when no voices are configured, the M3 path stays disabled and
    // LlmSentence events flow with no audio side-effect, identical to
    // M2 behaviour. The stack lives at function scope so its lifetime
    // brackets the FSM/Manager (started after, stopped before).
    std::unique_ptr<acva::playback::PlaybackQueue>  playback_queue;
    std::unique_ptr<acva::tts::OpenAiTtsClient>     openai_tts_client;
    std::unique_ptr<acva::playback::PlaybackEngine> playback_engine;
    std::unique_ptr<acva::audio::LoopbackSink>      loopback_sink;
    std::unique_ptr<acva::dialogue::TtsBridge>      tts_bridge;
    std::thread                                     playback_metrics_thread;
    std::atomic<bool>                               playback_metrics_stop{false};

    // Track the turn id the Manager mints for each LLM run. The
    // PlaybackEngine reads this to filter "live" chunks. Using
    // FSM.active_turn would be wrong for stdin mode: typed input
    // never fires SpeechStarted, so FSM stays in Listening with
    // active_turn=kNoTurn while Manager mints a fresh turn for the
    // LLM stream — chunks would be dropped as stale. M5+ will unify
    // turn minting between FSM and Manager (see manager.hpp).
    //
    // M6 — updated synchronously by Manager via its turn-started hook
    // (see set_turn_started_hook below), BEFORE LlmStarted publishes.
    // The earlier async bus subscriber was racy: when TTS chunks for
    // a new turn reached the queue before the LlmStarted subscriber
    // drained, dequeue_active dropped them all as stale — including
    // the EOS marker, which stranded the FSM in Speaking.
    auto playback_active_turn =
        std::make_shared<std::atomic<acva::event::TurnId>>(acva::event::kNoTurn);

    const bool tts_enabled = !cfg.tts.voices.empty();
    if (tts_enabled) {
        playback_queue = std::make_unique<acva::playback::PlaybackQueue>(
            cfg.playback.max_queue_chunks);
        playback_engine = std::make_unique<acva::playback::PlaybackEngine>(
            cfg.audio, cfg.playback, *playback_queue, bus,
            [playback_active_turn]{
                return playback_active_turn->load(std::memory_order_acquire);
            });

        // M6 — wire the AEC reference tap. The PlaybackEngine writes
        // every emitted chunk into this ring (with a wall-clock
        // timestamp); the M4 capture pipeline pulls aligned reference
        // frames from it for APM. We construct it whenever capture is
        // ALSO enabled — there's no point taping playback if no one
        // will read it. Sized in samples = ring_seconds × output rate.
        if (cfg.audio.capture_enabled) {
            const std::size_t loopback_capacity =
                static_cast<std::size_t>(cfg.audio.loopback.ring_seconds)
                * static_cast<std::size_t>(cfg.audio.sample_rate_hz);
            loopback_sink = std::make_unique<acva::audio::LoopbackSink>(
                loopback_capacity, cfg.audio.sample_rate_hz);
            playback_engine->set_loopback_sink(loopback_sink.get());
            acva::log::info("main", fmt::format(
                "loopback ring: {}s × {}Hz = {} samples",
                cfg.audio.loopback.ring_seconds,
                cfg.audio.sample_rate_hz,
                loopback_capacity));
        }

        // M4B: TTS goes through Speaches via OpenAiTtsClient. The
        // bridge takes a generic submit callable so the client class
        // doesn't leak beyond this block.
        openai_tts_client = std::make_unique<acva::tts::OpenAiTtsClient>(cfg.tts);
        acva::dialogue::TtsBridge::SubmitFn submit_fn =
            [c = openai_tts_client.get()](acva::tts::TtsRequest r,
                                            acva::tts::TtsCallbacks cb) {
                c->submit(std::move(r), std::move(cb));
            };
        acva::log::info("main", fmt::format(
            "tts (speaches): base_url={}", cfg.tts.base_url));

        tts_bridge = std::make_unique<acva::dialogue::TtsBridge>(
            cfg, bus, std::move(submit_fn), *playback_queue);

        playback_engine->start();
        tts_bridge->start();

        // Tiny poller thread: every 500 ms, push the engine + queue
        // counters into the metrics gauges. The audio thread can't
        // touch prometheus families directly (locks) so this side-thread
        // owns the bridging.
        playback_metrics_thread = std::thread([&]{
            using namespace std::chrono_literals;
            while (!playback_metrics_stop.load(std::memory_order_acquire)) {
                registry->set_playback_queue_depth(
                    static_cast<double>(playback_queue->size()));
                registry->set_playback_underruns_total(
                    static_cast<double>(playback_engine->underruns()));
                registry->set_playback_chunks_played_total(
                    static_cast<double>(playback_engine->chunks_played()));
                registry->set_playback_drops_total(
                    static_cast<double>(playback_queue->drops()));
                std::this_thread::sleep_for(500ms);
            }
        });

        acva::log::info("main", fmt::format(
            "tts enabled — voices configured: {} (audio: {} headless={})",
            cfg.tts.voices.size(),
            cfg.audio.output_device,
            playback_engine->headless()));
    } else {
        acva::log::info("main", "tts disabled — cfg.tts.voices is empty");
    }

    // Forward-declared so the /status closure can read APM stats at
    // request time. The pipeline is actually constructed below (M4
    // section) — we declare the unique_ptr early and the lambda picks
    // up its value lazily on every /status hit.
    std::unique_ptr<acva::audio::AudioPipeline> audio_pipeline;

    // Build the /status JSON-extra closure: services array + pipeline_state.
    // Lambda captures `&supervisor` by reference; lifetime is fine because
    // the ControlServer is destroyed before the supervisor.
    auto status_extra = [&supervisor, &audio_pipeline]() -> std::string {
        const auto snap = supervisor.snapshot();
        std::string out = "\"pipeline_state\":\"";
        out.append(acva::supervisor::to_string(snap.pipeline_state));
        out.append("\",\"services\":[");
        for (std::size_t i = 0; i < snap.services.size(); ++i) {
            const auto& s = snap.services[i];
            const auto last_ok_ms_ago =
                s.last_ok_at.time_since_epoch().count() == 0
                    ? -1
                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - s.last_ok_at).count();
            if (i) out.push_back(',');
            out += fmt::format(
                R"({{"name":"{}","state":"{}","last_ok_ms_ago":{},)"
                R"("consecutive_failures":{},"total_probes":{},)"
                R"("total_failures":{},"last_http_status":{}}})",
                s.name,
                acva::supervisor::to_string(s.state),
                last_ok_ms_ago,
                s.consecutive_failures,
                s.total_probes,
                s.total_failures,
                s.last_http_status);
        }
        out.push_back(']');

        // M6 — APM block. Present whenever the pipeline is up and an
        // APM stage was constructed (i.e., capture_enabled + a loopback
        // ring + a non-stub build of webrtc-audio-processing-1).
        if (audio_pipeline) {
            const auto* apm = audio_pipeline->apm();
            if (apm != nullptr) {
                const float erle = apm->erle_db();
                if (std::isnan(erle)) {
                    out += fmt::format(
                        R"(,"apm":{{"active":{},"delay_ms":{},)"
                        R"("erle_db":null,"frames_processed":{}}})",
                        apm->aec_active(),
                        apm->aec_delay_estimate_ms(),
                        apm->frames_processed());
                } else {
                    out += fmt::format(
                        R"(,"apm":{{"active":{},"delay_ms":{},)"
                        R"("erle_db":{:.2f},"frames_processed":{}}})",
                        apm->aec_active(),
                        apm->aec_delay_estimate_ms(),
                        erle,
                        apm->frames_processed());
                }
            }
        }
        return out;
    };

    // HTTP control plane (/metrics, /status, /health).
    std::unique_ptr<acva::http::ControlServer> control;
    try {
        control = std::make_unique<acva::http::ControlServer>(
            cfg.control, registry, &fsm, status_extra);
    } catch (const std::exception& ex) {
        acva::log::error("main", fmt::format("control server failed to start: {}", ex.what()));
        supervisor.stop();
        fsm.stop();
        bus.shutdown();
        return EXIT_FAILURE;
    }

    // ----- M4: capture + VAD pipeline -----
    // Constructed conditionally on cfg.audio.capture_enabled. When
    // disabled the M4 path stays dormant; the rest of the orchestrator
    // (M0–M3) behaves as before. When enabled, the fake driver
    // suppresses its synthetic SpeechStarted/SpeechEnded events so the
    // real VAD owns them — the rest of the synthetic pipeline still
    // runs (FinalTranscript / LlmSentence / TTS) until M5+ replaces it.
    std::unique_ptr<acva::audio::MonotonicAudioClock> audio_clock;
    std::unique_ptr<acva::audio::CaptureRing>          capture_ring;
    std::unique_ptr<acva::audio::CaptureEngine>        capture_engine;
    // audio_pipeline declared above so /status can read its APM stats.
    std::unique_ptr<acva::audio::HalfDuplexGate>       half_duplex_gate;
    std::thread                                         audio_metrics_thread;
    std::atomic<bool>                                   audio_metrics_stop{false};
    if (cfg.audio.capture_enabled) {
        audio_clock  = std::make_unique<acva::audio::MonotonicAudioClock>();
        capture_ring = std::make_unique<acva::audio::CaptureRing>();
        capture_engine = std::make_unique<acva::audio::CaptureEngine>(
            cfg.audio, *capture_ring, *audio_clock);

        // M5 half-duplex (speakers without AEC). When enabled, the
        // FSM informs the gate of Speaking transitions and the
        // capture engine drops mic samples while it's active. No-op
        // unless cfg.audio.half_duplex_while_speaking is true.
        if (cfg.audio.half_duplex_while_speaking) {
            half_duplex_gate = std::make_unique<acva::audio::HalfDuplexGate>(
                std::chrono::milliseconds{cfg.audio.half_duplex_hangover_ms});
            capture_engine->set_half_duplex_gate(half_duplex_gate.get());
            fsm.set_state_observer(
                [g = half_duplex_gate.get()](acva::dialogue::State prev,
                                              acva::dialogue::State next) {
                    const bool was_speaking = (prev == acva::dialogue::State::Speaking);
                    const bool is_speaking  = (next == acva::dialogue::State::Speaking);
                    if (was_speaking != is_speaking) {
                        g->set_speaking(is_speaking);
                    }
                });
            acva::log::info("main", fmt::format(
                "half-duplex mode enabled (hangover={}ms)",
                cfg.audio.half_duplex_hangover_ms));
        }

        acva::audio::AudioPipeline::Config apc;
        apc.input_sample_rate     = cfg.audio.sample_rate_hz;
        apc.output_sample_rate    = 16000;
        apc.endpointer.onset_threshold  = cfg.vad.onset_threshold;
        apc.endpointer.offset_threshold = cfg.vad.offset_threshold;
        apc.endpointer.min_speech_ms    = std::chrono::milliseconds{cfg.vad.min_speech_ms};
        apc.endpointer.hangover_ms      = std::chrono::milliseconds{cfg.vad.hangover_ms};
        apc.endpointer.pre_padding_ms   = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
        apc.endpointer.post_padding_ms  = std::chrono::milliseconds{cfg.vad.post_padding_ms};
        apc.pre_padding_ms       = std::chrono::milliseconds{cfg.vad.pre_padding_ms};
        apc.post_padding_ms      = std::chrono::milliseconds{cfg.vad.post_padding_ms};
        apc.max_in_flight        = cfg.utterance.max_in_flight;
        apc.max_duration_ms      = std::chrono::milliseconds{cfg.utterance.max_duration_ms};
        apc.vad_model_path       = cfg.vad.model_path;

        // M6 — install the AEC stage when both playback (loopback
        // ring) and an APM-capable build are present. cfg.apm controls
        // the per-subsystem switches; the wrapper is a no-op when the
        // webrtc-audio-processing-1 package wasn't found at build time.
        apc.loopback           = loopback_sink.get();
        apc.apm.aec_enabled    = cfg.apm.aec_enabled;
        apc.apm.ns_enabled     = cfg.apm.ns_enabled;
        apc.apm.agc_enabled    = cfg.apm.agc_enabled;
        apc.apm.initial_delay_estimate_ms =
            static_cast<int>(cfg.apm.initial_delay_estimate_ms);
        apc.apm.max_delay_ms   = static_cast<int>(cfg.apm.max_delay_ms);

        audio_pipeline = std::make_unique<acva::audio::AudioPipeline>(
            std::move(apc), *capture_ring, *audio_clock, bus);

        // M6 — warm Whisper into VRAM before opening the mic. Without
        // this the first user turn pays the full model-load cost
        // (3-5 s for medium / large-v3 / int8_float16 quantised
        // models), which manifests as a long silence after the user
        // stops speaking. Synchronous + best-effort: errors are logged
        // and ignored so a warm-up failure doesn't block the rest of
        // the orchestrator coming up.
        if (cfg.stt.warmup_on_startup && !cfg.stt.base_url.empty()) {
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

        capture_engine->start();
        audio_pipeline->start();

        // Mirror M4 counters into /metrics every 500 ms. Same shape as
        // the playback poller below — main owns the bridge so the
        // capture audio thread never touches prometheus families.
        audio_metrics_thread = std::thread([&]{
            using namespace std::chrono_literals;
            while (!audio_metrics_stop.load(std::memory_order_acquire)) {
                registry->set_capture_frames_total(
                    static_cast<double>(capture_engine->frames_captured()));
                registry->set_capture_ring_overruns_total(
                    static_cast<double>(capture_engine->ring_overruns()));
                registry->set_audio_pipeline_frames_total(
                    static_cast<double>(audio_pipeline->frames_processed()));
                registry->set_vad_false_starts_total(
                    static_cast<double>(audio_pipeline->false_starts_total()));
                registry->set_utterances_total(
                    static_cast<double>(audio_pipeline->utterances_total()));
                registry->set_utterance_drops_total(
                    static_cast<double>(audio_pipeline->utterance_drops()));
                registry->set_utterance_in_flight(
                    static_cast<double>(audio_pipeline->in_flight()));

                // M6 — surface APM stats whenever the AEC stage is wired
                // in. Runs the gauges off the same poller so we don't
                // need a second thread; the 500 ms cadence is fine
                // because APM's internal estimator updates over seconds.
                if (const auto* apm = audio_pipeline->apm(); apm != nullptr) {
                    registry->set_aec_delay_estimate_ms(
                        static_cast<double>(apm->aec_delay_estimate_ms()));
                    registry->set_aec_erle_db(
                        static_cast<double>(apm->erle_db()));
                    registry->set_aec_frames_processed_total(
                        static_cast<double>(apm->frames_processed()));
                }
                std::this_thread::sleep_for(500ms);
            }
        });

        acva::log::info("main", fmt::format(
            "audio capture enabled (input='{}', headless={}, vad={})",
            cfg.audio.input_device,
            capture_engine->headless(),
            audio_pipeline->vad_enabled() ? "silero" : "off"));
    } else {
        acva::log::info("main", "audio capture disabled");
    }

    // ----- STT — two paths -----
    //
    // M5 streaming (cfg.stt.streaming = true, default): RealtimeSttClient
    // owns one long-lived WebRTC session against /v1/realtime. The
    // capture pipeline pushes 16 kHz mono chunks into the live sink
    // between SpeechStarted and SpeechEnded; the client publishes
    // PartialTranscript / FinalTranscript on the bus.
    //
    // M4B request/response (cfg.stt.streaming = false): OpenAiSttClient
    // POSTs each UtteranceReady's audio buffer to
    // /v1/audio/transcriptions. Used by fixture demos and as a
    // fallback when libdatachannel isn't available.
    //
    // Both publish FinalTranscript on the same bus; the dialogue
    // Manager consumes it without caring which path produced it.
    std::unique_ptr<acva::stt::RealtimeSttClient> realtime_stt;
    std::unique_ptr<acva::stt::OpenAiSttClient>   stt_client;
    std::thread                                    stt_worker;
    std::atomic<bool>                              stt_stop{false};
    std::mutex                                     stt_mu;
    std::condition_variable                        stt_cv;
    std::deque<acva::stt::SttRequest>              stt_queue;

    if (!cfg.stt.base_url.empty() && cfg.stt.streaming && cfg.audio.capture_enabled) {
        realtime_stt = std::make_unique<acva::stt::RealtimeSttClient>(cfg.stt);
        const bool ok = realtime_stt->start(std::chrono::seconds(15));
        if (!ok) {
            acva::log::warn("main",
                "realtime stt failed to start; dialogue path will not "
                "receive transcripts (check `acva demo health` for Speaches)");
            realtime_stt.reset();
        } else {
            // Wire the live audio sink — pipeline pushes chunks, client
            // bridges to Speaches over the data channel. Only when
            // capture is enabled; otherwise the sink stays disconnected
            // (the synthetic fake-driver path doesn't go through STT).
            if (audio_pipeline) {
                audio_pipeline->set_live_audio_sink(
                    [&realtime_stt](std::span<const std::int16_t> samples) {
                        if (realtime_stt) realtime_stt->push_audio(samples);
                    });
            }

            // begin_utterance on SpeechStarted, end_utterance on
            // SpeechEnded. Callbacks publish PartialTranscript /
            // FinalTranscript on the bus so the dialogue Manager
            // consumes them transparently. turn=NoTurn — the FSM
            // doesn't validate turn ids on these events.
            auto sub_started = bus.subscribe<acva::event::SpeechStarted>({},
                [&realtime_stt, &bus](const acva::event::SpeechStarted&) {
                    if (!realtime_stt) return;
                    auto cancel = std::make_shared<acva::dialogue::CancellationToken>();
                    acva::stt::RealtimeSttClient::UtteranceCallbacks cb;
                    cb.on_partial = [&bus](acva::event::PartialTranscript p) {
                        bus.publish(std::move(p));
                    };
                    cb.on_final = [&bus](acva::event::FinalTranscript f) {
                        bus.publish(std::move(f));
                    };
                    cb.on_error = [](std::string err) {
                        acva::log::warn("stt",
                            fmt::format("realtime transcription failed: {}", err));
                    };
                    realtime_stt->begin_utterance(
                        acva::event::kNoTurn, cancel, std::move(cb));
                });
            auto sub_ended = bus.subscribe<acva::event::SpeechEnded>({},
                [&realtime_stt](const acva::event::SpeechEnded&) {
                    if (realtime_stt) realtime_stt->end_utterance();
                });
            metric_subs.push_back(sub_started);
            metric_subs.push_back(sub_ended);

            acva::log::info("main", fmt::format(
                "stt enabled (streaming, base_url={}, model={})",
                cfg.stt.base_url, cfg.stt.model));
        }
    } else if (!cfg.stt.base_url.empty()) {
        stt_client = std::make_unique<acva::stt::OpenAiSttClient>(cfg.stt);

        auto stt_sub = bus.subscribe<acva::event::UtteranceReady>({},
            [&](const acva::event::UtteranceReady& e) {
                if (!e.slice) return;
                std::lock_guard lk(stt_mu);
                stt_queue.push_back(acva::stt::SttRequest{
                    .turn  = e.turn,
                    .slice = e.slice,
                    .cancel = std::make_shared<acva::dialogue::CancellationToken>(),
                    .lang_hint = cfg.stt.language,
                });
                stt_cv.notify_one();
            });
        metric_subs.push_back(stt_sub);

        stt_worker = std::thread([&] {
            while (!stt_stop.load(std::memory_order_acquire)) {
                acva::stt::SttRequest req;
                {
                    std::unique_lock lk(stt_mu);
                    stt_cv.wait(lk, [&]{
                        return stt_stop.load(std::memory_order_acquire)
                                || !stt_queue.empty();
                    });
                    if (stt_stop.load(std::memory_order_acquire)) break;
                    req = std::move(stt_queue.front());
                    stt_queue.pop_front();
                }
                acva::stt::SttCallbacks cb;
                cb.on_final = [&bus](acva::event::FinalTranscript ft) {
                    bus.publish(std::move(ft));
                };
                cb.on_error = [](std::string err) {
                    acva::log::warn("stt",
                        fmt::format("transcription failed: {}", err));
                };
                stt_client->submit(std::move(req), std::move(cb));
            }
        });

        acva::log::info("main", fmt::format(
            "stt enabled (request/response, base_url={}, model={})",
            cfg.stt.base_url, cfg.stt.model));
    } else {
        acva::log::info("main", "stt disabled (cfg.stt.base_url empty)");
    }

    // Optional fake pipeline driver (M0). Mutually exclusive with the
    // M1 LLM stack — see below.
    std::unique_ptr<acva::pipeline::FakeDriver> fake_driver;
    if (cfg.pipeline.fake_driver_enabled) {
        acva::pipeline::FakeDriverOptions opts;
        opts.sentences_per_turn = cfg.pipeline.fake_sentences_per_turn;
        opts.idle_between_turns = std::chrono::milliseconds{cfg.pipeline.fake_idle_between_turns_ms};
        opts.barge_in_probability = cfg.pipeline.fake_barge_in_probability;
        opts.suppress_speech_events = cfg.audio.capture_enabled;
        fake_driver = std::make_unique<acva::pipeline::FakeDriver>(bus, opts);
        fake_driver->start();
        acva::log::info("main", "fake pipeline driver enabled");
    } else {
        acva::log::info("main", "fake pipeline driver disabled");
    }

    // ----- LLM / dialogue stack -----
    //
    // Constructed whenever an LLM is configured (cfg.llm.base_url non-empty).
    // Drives the dialogue path for both `--stdin` text input AND the
    // M5 capture+STT path that publishes FinalTranscript on the bus.
    // Without this stack, FinalTranscript events have no consumer and
    // the FSM gets stuck in `thinking` after the first transcript.
    std::unique_ptr<acva::llm::PromptBuilder> prompt_builder;
    std::unique_ptr<acva::llm::LlmClient> llm_client;
    std::unique_ptr<acva::dialogue::Manager> manager;
    std::unique_ptr<acva::dialogue::TurnWriter> turn_writer;
    std::unique_ptr<acva::memory::Summarizer> summarizer;
    std::unique_ptr<acva::supervisor::KeepAlive> keep_alive;
    std::thread stdin_reader;

    if (!cfg.llm.base_url.empty()) {
        auto sid_or = memory->read([](acva::memory::Repository& repo) {
            return repo.insert_session(acva::memory::now_ms(), std::nullopt);
        });
        if (auto* err = std::get_if<acva::memory::DbError>(&sid_or)) {
            acva::log::error("main", fmt::format("session insert failed: {}", err->message));
            fsm.stop();
            bus.shutdown();
            return EXIT_FAILURE;
        }
        const auto session_id = std::get<acva::memory::SessionId>(sid_or);
        acva::log::event("main", "session_opened", acva::event::kNoTurn,
                          {{"session_id", std::to_string(session_id)}});

        prompt_builder = std::make_unique<acva::llm::PromptBuilder>(cfg, *memory);
        llm_client     = std::make_unique<acva::llm::LlmClient>(cfg, bus);
        manager        = std::make_unique<acva::dialogue::Manager>(
                            cfg, bus, *prompt_builder, *llm_client, turns);
        turn_writer    = std::make_unique<acva::dialogue::TurnWriter>(bus, *memory);
        summarizer     = std::make_unique<acva::memory::Summarizer>(
                            cfg, bus, *memory, *llm_client);
        manager->set_session(session_id);
        turn_writer->set_session(session_id);
        summarizer->set_session(session_id);

        // M2: pipeline gating + LLM keep-alive. The gate refuses new
        // turns when the supervisor reports pipeline_state==Failed; the
        // keep-alive timer pings llama every keep_alive_interval_seconds
        // while the FSM is Listening so the model stays loaded.
        manager->set_pipeline_gate([&supervisor]{
            return supervisor.pipeline_state()
                != acva::supervisor::PipelineState::Failed;
        });

        // Manager adopts the FSM's already-minted turn id (the one
        // minted on `speech_started`) so PlaybackFinished events that
        // carry the id all the way back to the FSM match
        // `Fsm::active_.id`. Without this, FSM and Manager mint
        // separate ids for the same logical turn and FSM rejects
        // every PlaybackFinished as stale.
        manager->set_active_turn_provider([&fsm]{
            return fsm.snapshot().active_turn;
        });

        // Synchronously bump playback_active_turn the instant Manager
        // mints/adopts the turn id, before LlmStarted publishes. This
        // closes the race that previously stranded the FSM in
        // Speaking — see playback_active_turn declaration above.
        manager->set_turn_started_hook(
            [playback_active_turn](acva::event::TurnId t) {
                playback_active_turn->store(t, std::memory_order_release);
            });

        keep_alive = std::make_unique<acva::supervisor::KeepAlive>(
            acva::supervisor::KeepAlive::Options{
                .interval = std::chrono::milliseconds(
                    cfg.llm.keep_alive_interval_seconds * 1000ULL),
                .should_fire = [&fsm]{
                    return fsm.snapshot().state == acva::dialogue::State::Listening;
                },
                .on_tick = [client = llm_client.get()]{ client->keep_alive(); },
                .on_fired   = [registry]{ registry->on_keep_alive(/*fired*/ true); },
                .on_skipped = [registry]{ registry->on_keep_alive(/*fired*/ false); },
            });

        manager->start();
        turn_writer->start();
        summarizer->start();
        keep_alive->start();

        if (!llm_client->probe()) {
            acva::log::info("main",
                "llama /health probe failed; will still attempt requests");
        }

        // Echo streamed sentences to the terminal for visual confirmation.
        // Useful in both stdin and mic-driven modes.
        auto stdout_sub = bus.subscribe<acva::event::LlmSentence>({},
            [](const acva::event::LlmSentence& e) {
                std::cout << "  " << e.text << "\n" << std::flush;
            });
        metric_subs.push_back(stdout_sub);
    } else {
        acva::log::info("main", "llm disabled (cfg.llm.base_url empty); "
                                "FinalTranscript events will have no consumer");
    }

    // ----- stdin text-input mode (M1 era) — only the line reader -----
    if (args.stdin_mode) {
        std::cout << "acva stdin mode — type a line and press enter. Ctrl-D or Ctrl-C to exit.\n";
        stdin_reader = std::thread([&bus, &cfg]{
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty()) continue;
                bus.publish(acva::event::FinalTranscript{
                    .turn = 0,
                    .text = line,
                    .lang = cfg.dialogue.fallback_language,
                    .confidence = 1.0F,
                    .audio_duration = {},
                    .processing_duration = {},
                });
            }
            // EOF on stdin (Ctrl-D or stdin closed) — request shutdown.
            g_signal_received.store(SIGTERM);
        });
    }

    // ----- 4. Main loop -----
    while (g_signal_received.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int sig = g_signal_received.load();
    acva::log::info("main", fmt::format("received signal {}, shutting down", sig));

    // Wake the stdin reader if it's still in getline().
    if (stdin_reader.joinable()) {
        ::close(STDIN_FILENO);
        stdin_reader.join();
    }

    // Orderly shutdown: stop producers first, then drain.
    if (fake_driver) {
        fake_driver->stop();
    }
    // M4: stop capture before the pipeline so no more frames land in
    // the SPSC ring while the worker thread is winding down. Then
    // wind the metrics poller down so it stops reading from the
    // engines as they tear down.
    if (capture_engine) capture_engine->stop();
    if (audio_pipeline) audio_pipeline->stop();
    if (audio_metrics_thread.joinable()) {
        audio_metrics_stop.store(true, std::memory_order_release);
        audio_metrics_thread.join();
    }
    // M5: tear down the realtime STT before the audio pipeline so no
    // chunks try to push onto a dead data channel.
    if (realtime_stt) {
        realtime_stt->stop();
    }
    // M4B: stop the STT worker after the pipeline so no late
    // UtteranceReady events queue up unprocessed.
    if (stt_worker.joinable()) {
        stt_stop.store(true, std::memory_order_release);
        stt_cv.notify_all();
        stt_worker.join();
    }
    if (keep_alive)  keep_alive->stop();   // before llm_client teardown
    if (manager)     manager->stop();
    if (turn_writer) turn_writer->stop();
    if (summarizer)  summarizer->stop();
    // M3: stop tts producer first so no new chunks land in the queue,
    // then drain the engine, then everything else.
    if (tts_bridge)     tts_bridge->stop();
    if (playback_engine) playback_engine->stop();
    if (tts_enabled) {
        playback_metrics_stop.store(true, std::memory_order_release);
        if (playback_metrics_thread.joinable()) playback_metrics_thread.join();
    }
    supervisor.stop();
    fsm.stop();
    control.reset();
    bus.shutdown();

    acva::log::info("main", "acva exited cleanly");
    return EXIT_SUCCESS;
}
