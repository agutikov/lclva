#include "demos/demo.hpp"

#include "config/paths.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/manager.hpp"
#include "dialogue/tts_bridge.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "memory/memory_thread.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "tts/piper_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

namespace acva::demos {

namespace {

// Locate a tmp SQLite path so the demo doesn't pollute the user's
// production memory.db. The MemoryThread + recovery sweep need *some*
// path, but we don't care about persistence for a one-shot demo.
std::filesystem::path tmp_db_path() {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path()
           / (std::string{"acva-demo-chat-"}
              + std::to_string(::getpid()) + ".db");
    fs::remove(p);
    return p;
}

} // namespace

int run_chat(const config::Config& orig_cfg) {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    constexpr std::string_view kPrompt =
        "Say hello in one short sentence and mention the moon.";

    if (orig_cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[chat] FAIL: cfg.tts.voices is empty — populate it before running\n");
        return EXIT_FAILURE;
    }

    // Local copy so we can redirect the SQLite path at a tmp file.
    config::Config cfg = orig_cfg;
    cfg.memory.db_path = tmp_db_path().string();

    std::printf(
        "demo[chat] llm=%s tts.voice='%s' device='%s' prompt=\"%.*s\"\n",
        cfg.llm.base_url.c_str(),
        cfg.tts.fallback_lang.c_str(),
        cfg.audio.output_device.c_str(),
        static_cast<int>(kPrompt.size()), kPrompt.data());

    event::EventBus bus;

    // Memory: needed by PromptBuilder; the demo runs with empty
    // history (no recent_turns / no facts / no summary), and the
    // tmp file is cleaned at teardown.
    auto mem_or = memory::MemoryThread::open(cfg.memory.db_path,
                                                cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<memory::DbError>(&mem_or)) {
        std::fprintf(stderr, "demo[chat] memory open: %s\n", err->message.c_str());
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<memory::MemoryThread>>(mem_or));

    // LLM: probe the backend before spinning everything up. The demo
    // is allowed to fail fast on a missing llama-server.
    llm::LlmClient client(cfg, bus);
    if (!client.probe()) {
        std::fprintf(stderr,
            "demo[chat] FAIL: llama /health probe failed at %s — is the "
            "compose stack up?\n", cfg.llm.base_url.c_str());
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }

    llm::PromptBuilder       pb(cfg, *memory);
    dialogue::TurnFactory    turns;
    dialogue::Fsm            fsm(bus, turns);
    dialogue::Manager        manager(cfg, bus, pb, client, turns);

    playback::PlaybackQueue  queue(cfg.playback.max_queue_chunks);
    tts::PiperClient         piper(cfg.tts);

    // Track the turn id the Manager mints for the LLM run. The
    // PlaybackEngine needs to know which turn's chunks are "live" so
    // it doesn't filter them out. FSM's own active_turn is set on
    // SpeechStarted (which we don't emit here) so we'd otherwise read
    // kNoTurn and the engine would drop every chunk.
    std::atomic<event::TurnId> playback_turn{event::kNoTurn};

    auto sub_started = bus.subscribe<event::LlmStarted>({},
        [&](const event::LlmStarted& e) {
            playback_turn.store(e.turn, std::memory_order_release);
        });

    playback::PlaybackEngine engine(cfg.audio, queue, bus,
                                      [&]{ return playback_turn.load(std::memory_order_acquire); });
    dialogue::TtsBridge      bridge(cfg, bus, piper, queue);

    // Echo every sentence to stdout for the user, mirroring how
    // `--stdin` mode prints them.
    auto sub_sentence = bus.subscribe<event::LlmSentence>({},
        [](const event::LlmSentence& e) {
            std::cout << "  " << e.text << "\n" << std::flush;
        });

    // Capture LlmFinished so we know when to stop waiting.
    std::atomic<bool> llm_finished{false};
    std::atomic<std::size_t> tokens{0};
    std::atomic<bool> llm_cancelled{false};
    auto sub_finished = bus.subscribe<event::LlmFinished>({},
        [&](const event::LlmFinished& e) {
            tokens.store(e.tokens_generated, std::memory_order_relaxed);
            llm_cancelled.store(e.cancelled, std::memory_order_relaxed);
            llm_finished.store(true, std::memory_order_release);
        });

    fsm.start();
    if (!engine.start()) {
        std::fprintf(stderr, "demo[chat] engine.start() failed\n");
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    bridge.start();
    manager.start();
    std::printf("demo[chat] stack up (audio headless=%s); submitting prompt\n\n",
                 engine.headless() ? "true" : "false");

    const auto t0 = steady_clock::now();
    bus.publish(event::FinalTranscript{
        .turn                = event::kNoTurn,
        .text                = std::string{kPrompt},
        .lang                = cfg.tts.fallback_lang,
        .confidence          = 1.0F,
        .audio_duration      = {},
        .processing_duration = {},
    });

    // Wait for the LLM to finish streaming. Generous deadline so a
    // cold-loaded Qwen2.5-7B doesn't trip the demo on its first run.
    const auto llm_deadline = t0 + 60s;
    while (!llm_finished.load(std::memory_order_acquire)
           && steady_clock::now() < llm_deadline) {
        std::this_thread::sleep_for(20ms);
    }
    const auto t_llm_done = steady_clock::now();

    if (!llm_finished.load()) {
        std::fprintf(stderr,
            "demo[chat] FAIL: LlmFinished never fired (timeout). "
            "Check `docker compose logs llama` for cold-start errors.\n");
        manager.stop(); bridge.stop(); engine.stop(); fsm.stop();
        bus.shutdown();
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }

    // Wait for the playback queue to drain. The bridge keeps
    // synthesizing for a beat after the last sentence arrives, then
    // the engine consumes the final chunks.
    const auto play_deadline = t_llm_done + 30s;
    while ((queue.size() > 0 || engine.chunks_played() == 0)
           && steady_clock::now() < play_deadline) {
        std::this_thread::sleep_for(50ms);
    }
    // Settle the trailing chunk so the user actually hears it.
    std::this_thread::sleep_for(250ms);

    const auto t_done = steady_clock::now();
    const auto llm_ms  = duration<double, std::milli>(t_llm_done - t0).count();
    const auto tot_ms  = duration<double, std::milli>(t_done    - t0).count();

    manager.stop();
    bridge.stop();
    engine.stop();
    sub_started->stop();
    sub_sentence->stop();
    sub_finished->stop();
    fsm.stop();
    bus.shutdown();
    std::filesystem::remove(cfg.memory.db_path);

    std::printf(
        "\ndemo[chat] done: tokens=%zu llm_ms=%.1f total_ms=%.1f "
        "synthesized=%llu chunks_played=%llu underruns=%llu\n",
        tokens.load(),
        llm_ms, tot_ms,
        static_cast<unsigned long long>(bridge.sentences_synthesized()),
        static_cast<unsigned long long>(engine.chunks_played()),
        static_cast<unsigned long long>(engine.underruns()));

    if (engine.headless()) {
        std::printf(
            "demo[chat] NOTE: engine ran headless — synthesis verified, "
            "no audio reached the speakers.\n");
    }
    if (llm_cancelled.load()) {
        // Cap-triggered cancel is normal for very long replies; the
        // sentences that did emit still played. Informational.
        std::printf(
            "demo[chat] NOTE: LLM stream was cancelled "
            "(probably hit max_assistant_sentences=%u).\n",
            cfg.dialogue.max_assistant_sentences);
    }
    if (tokens.load() == 0) {
        std::fprintf(stderr,
            "demo[chat] FAIL: zero tokens received from llama.\n");
        return EXIT_FAILURE;
    }
    if (bridge.sentences_synthesized() == 0) {
        std::fprintf(stderr,
            "demo[chat] FAIL: bridge synthesized zero sentences — Piper "
            "may be unreachable at the configured voice URL.\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
