#include "audio/capture.hpp"
#include "audio/clock.hpp"
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
#include "supervisor/keep_alive.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"
#include "supervisor/supervisor.hpp"
#include "tts/openai_tts_client.hpp"
#include "tts/piper_client.hpp"

#include <fmt/format.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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
    // Empty → ${XDG_DATA_HOME}/acva/models/silero_vad.onnx (the path
    // scripts/download-silero-vad.sh writes to). If the file isn't
    // there, AudioPipeline catches the load failure and disables VAD
    // with a warning — so leaving model_path unset is safe.
    if (cfg.vad.model_path.empty()) {
        const auto resolved = acva::config::resolve_data_path(
            "", "models/silero_vad.onnx");
        if (std::filesystem::exists(resolved)) {
            cfg.vad.model_path = resolved.string();
        }
    } else {
        cfg.vad.model_path =
            acva::config::resolve_data_path(cfg.vad.model_path, "silero_vad.onnx").string();
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
    std::unique_ptr<acva::tts::PiperClient>         piper_client;
    std::unique_ptr<acva::tts::OpenAiTtsClient>     openai_tts_client;
    std::unique_ptr<acva::playback::PlaybackEngine> playback_engine;
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
    auto playback_active_turn =
        std::make_shared<std::atomic<acva::event::TurnId>>(acva::event::kNoTurn);
    auto llm_started_sub = bus.subscribe<acva::event::LlmStarted>({},
        [playback_active_turn](const acva::event::LlmStarted& e) {
            playback_active_turn->store(e.turn, std::memory_order_release);
        });
    metric_subs.push_back(llm_started_sub);

    const bool tts_enabled = !cfg.tts.voices.empty();
    if (tts_enabled) {
        playback_queue = std::make_unique<acva::playback::PlaybackQueue>(
            cfg.playback.max_queue_chunks);
        playback_engine = std::make_unique<acva::playback::PlaybackEngine>(
            cfg.audio, *playback_queue, bus,
            [playback_active_turn]{
                return playback_active_turn->load(std::memory_order_acquire);
            });

        // M4B: pick the TTS client based on cfg.tts.provider.
        //   "speaches" → OpenAiTtsClient against cfg.tts.base_url
        //   "piper"    → legacy PiperClient (pre-M4B path; deleted in Step 6)
        // The bridge takes a generic submit callable so neither client
        // class leaks beyond this block.
        acva::dialogue::TtsBridge::SubmitFn submit_fn;
        if (cfg.tts.provider == "speaches") {
            openai_tts_client = std::make_unique<acva::tts::OpenAiTtsClient>(cfg.tts);
            submit_fn = [c = openai_tts_client.get()]
                          (acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
                c->submit(std::move(r), std::move(cb));
            };
            acva::log::info("main", fmt::format(
                "tts provider=speaches, base_url={}", cfg.tts.base_url));
        } else {
            piper_client = std::make_unique<acva::tts::PiperClient>(cfg.tts);
            submit_fn = [c = piper_client.get()]
                          (acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
                c->submit(std::move(r), std::move(cb));
            };
            acva::log::info("main", "tts provider=piper (legacy)");
        }
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

    // Build the /status JSON-extra closure: services array + pipeline_state.
    // Lambda captures `&supervisor` by reference; lifetime is fine because
    // the ControlServer is destroyed before the supervisor.
    auto status_extra = [&supervisor]() -> std::string {
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
    std::unique_ptr<acva::audio::AudioPipeline>        audio_pipeline;
    std::thread                                         audio_metrics_thread;
    std::atomic<bool>                                   audio_metrics_stop{false};
    if (cfg.audio.capture_enabled) {
        audio_clock  = std::make_unique<acva::audio::MonotonicAudioClock>();
        capture_ring = std::make_unique<acva::audio::CaptureRing>();
        capture_engine = std::make_unique<acva::audio::CaptureEngine>(
            cfg.audio, *capture_ring, *audio_clock);

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

        audio_pipeline = std::make_unique<acva::audio::AudioPipeline>(
            std::move(apc), *capture_ring, *audio_clock, bus);

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

    // ----- M4B: STT — UtteranceReady → POST /v1/audio/transcriptions -----
    //
    // Constructed conditionally on cfg.stt.base_url. When set, every
    // UtteranceReady from the M4 pipeline is dispatched to a
    // single-thread STT executor and the resulting FinalTranscript is
    // republished on the bus. The dialogue Manager already consumes
    // FinalTranscript — no FSM changes needed.
    //
    // M4B uses request/response (Speaches'
    // /v1/audio/transcriptions). M5 will swap the inner client for
    // /v1/realtime streaming.
    std::unique_ptr<acva::stt::OpenAiSttClient> stt_client;
    std::thread                                  stt_worker;
    std::atomic<bool>                            stt_stop{false};
    std::mutex                                   stt_mu;
    std::condition_variable                      stt_cv;
    std::deque<acva::stt::SttRequest>            stt_queue;
    if (!cfg.stt.base_url.empty()) {
        stt_client = std::make_unique<acva::stt::OpenAiSttClient>(cfg.stt);

        auto stt_sub = bus.subscribe<acva::event::UtteranceReady>({},
            [&](const acva::event::UtteranceReady& e) {
                if (!e.slice) return;
                std::lock_guard lk(stt_mu);
                stt_queue.push_back(acva::stt::SttRequest{
                    .turn  = e.turn,
                    .slice = e.slice,
                    .cancel = std::make_shared<acva::dialogue::CancellationToken>(),
                    .lang_hint = "",
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
            "stt enabled (base_url={}, model={})",
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

    // ----- M1 stdin mode: real LLM stack driven by stdin lines -----
    std::unique_ptr<acva::llm::PromptBuilder> prompt_builder;
    std::unique_ptr<acva::llm::LlmClient> llm_client;
    std::unique_ptr<acva::dialogue::Manager> manager;
    std::unique_ptr<acva::dialogue::TurnWriter> turn_writer;
    std::unique_ptr<acva::memory::Summarizer> summarizer;
    std::unique_ptr<acva::supervisor::KeepAlive> keep_alive;
    std::thread stdin_reader;

    if (args.stdin_mode) {
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
        auto stdout_sub = bus.subscribe<acva::event::LlmSentence>({},
            [](const acva::event::LlmSentence& e) {
                std::cout << "  " << e.text << "\n" << std::flush;
            });
        metric_subs.push_back(stdout_sub);

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
