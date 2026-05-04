#include "demos/demo.hpp"

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
#include "tts/openai_tts_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <variant>

namespace acva::demos {

namespace {

std::filesystem::path tmp_db_path() {
    namespace fs = std::filesystem;
    auto p = fs::temp_directory_path()
           / (std::string{"acva-demo-bargein-"}
              + std::to_string(::getpid()) + ".db");
    fs::remove(p);
    return p;
}

} // namespace

// `acva demo bargein` — exercises the M7 cancellation cascade end-to-end
// without touching the mic. Same wiring as `demo chat`, but instead of
// waiting for the LLM to drain, the demo programmatically publishes
// UserInterrupted ~250 ms after the first sentence starts playing and
// asserts that:
//   • Manager cancels the LLM stream (LlmFinished{cancelled=true}).
//   • PlaybackQueue drains stale chunks.
//   • PlaybackEngine emits silence within ≤ 400 ms (M7 §Demo).
//
// What it does NOT cover: real-mic-driven barge-in (the BargeInDetector
// + AEC gate). That's `acva demo capture` + `aec-record` + the manual
// 50-trial test in plans/milestones/m7_barge_in.md §5.
int run_bargein(const config::Config& orig_cfg) {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    constexpr std::string_view kPrompt =
        "Tell me a four-sentence story about the moon. Make every sentence "
        "complete on its own.";

    if (orig_cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: cfg.tts.voices is empty — populate it before running\n");
        return EXIT_FAILURE;
    }

    config::Config cfg = orig_cfg;
    cfg.memory.db_path = tmp_db_path().string();

    std::printf(
        "demo[bargein] llm=%s tts.voice='%s' device='%s'\n"
        "demo[bargein] submitting long prompt; will inject interrupt after first sentence plays\n\n",
        cfg.llm.base_url.c_str(),
        cfg.tts.fallback_lang.c_str(),
        cfg.audio.output_device.c_str());

    event::EventBus bus;

    auto mem_or = memory::MemoryThread::open(cfg.memory.db_path,
                                              cfg.memory.write_queue_capacity);
    if (auto* err = std::get_if<memory::DbError>(&mem_or)) {
        std::fprintf(stderr, "demo[bargein] memory open: %s\n", err->message.c_str());
        return EXIT_FAILURE;
    }
    auto memory = std::move(std::get<std::unique_ptr<memory::MemoryThread>>(mem_or));

    llm::LlmClient client(cfg, bus);
    if (!client.probe()) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: llama /health probe failed at %s\n",
            cfg.llm.base_url.c_str());
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }

    llm::PromptBuilder       pb(cfg, *memory);
    dialogue::TurnFactory    turns;
    dialogue::Fsm            fsm(bus, turns);
    dialogue::Manager        manager(cfg, bus, pb, client, turns);

    playback::PlaybackQueue  queue(cfg.playback.max_queue_chunks);
    tts::OpenAiTtsClient     tts_client(cfg.tts);

    std::atomic<event::TurnId> playback_turn{event::kNoTurn};
    auto sub_started = bus.subscribe<event::LlmStarted>({},
        [&](const event::LlmStarted& e) {
            playback_turn.store(e.turn, std::memory_order_release);
        });

    playback::PlaybackEngine engine(cfg.audio, cfg.playback, queue, bus,
                                      [&]{ return playback_turn.load(std::memory_order_acquire); });
    dialogue::TtsBridge      bridge(cfg, bus,
        [&](tts::TtsRequest r, tts::TtsCallbacks cb) {
            tts_client.submit(std::move(r), std::move(cb));
        }, queue);

    auto sub_sentence = bus.subscribe<event::LlmSentence>({},
        [](const event::LlmSentence& e) {
            std::cout << "  " << e.text << "\n" << std::flush;
        });

    // Track sentence playback for the persistence-policy report.
    std::atomic<std::size_t> sentences_emitted{0};
    auto sub_emitted = bus.subscribe<event::LlmSentence>({},
        [&](const event::LlmSentence&) {
            sentences_emitted.fetch_add(1, std::memory_order_relaxed);
        });
    std::atomic<std::size_t> sentences_played{0};
    std::atomic<event::TurnId> first_played_turn{event::kNoTurn};
    auto sub_pf = bus.subscribe<event::PlaybackFinished>({},
        [&](const event::PlaybackFinished& e) {
            sentences_played.fetch_add(1, std::memory_order_relaxed);
            event::TurnId expected = event::kNoTurn;
            first_played_turn.compare_exchange_strong(
                expected, e.turn, std::memory_order_acq_rel);
        });

    std::atomic<bool> llm_finished{false};
    std::atomic<bool> llm_cancelled{false};
    auto sub_finished = bus.subscribe<event::LlmFinished>({},
        [&](const event::LlmFinished& e) {
            llm_cancelled.store(e.cancelled, std::memory_order_relaxed);
            llm_finished.store(true, std::memory_order_release);
        });

    fsm.start();
    if (!engine.start()) {
        std::fprintf(stderr, "demo[bargein] engine.start() failed\n");
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    bridge.start();
    manager.start();
    std::printf("demo[bargein] stack up (audio headless=%s); submitting prompt\n",
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

    // Wait for the first sentence to start playing — at least one
    // PlaybackFinished, OR a generous fallback so we don't hang on a
    // headless engine.
    const auto inject_after_first = t0 + 30s;
    while (sentences_played.load(std::memory_order_acquire) == 0
           && steady_clock::now() < inject_after_first) {
        std::this_thread::sleep_for(20ms);
    }
    if (sentences_played.load() == 0) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: no sentence reached PlaybackFinished within 30s — "
            "either TTS is slow or the audio engine is silently stuck.\n");
        manager.stop(); bridge.stop(); engine.stop(); fsm.stop();
        bus.shutdown();
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }

    // Inject UserInterrupted. Use the FSM's active turn so the cascade
    // routes through the right id.
    const auto target_turn = playback_turn.load(std::memory_order_acquire);
    const auto sentences_played_at_inject =
        sentences_played.load(std::memory_order_relaxed);
    const auto inject_at = steady_clock::now();
    std::printf("demo[bargein] injecting UserInterrupted (turn=%llu, played=%zu sentence%s)\n",
                 static_cast<unsigned long long>(target_turn),
                 sentences_played_at_inject,
                 sentences_played_at_inject == 1 ? "" : "s");
    bus.publish(event::UserInterrupted{
        .turn = target_turn,
        .ts   = inject_at,
    });

    // Wait for the cascade: LlmFinished{cancelled} + queue drain.
    const auto cancel_deadline = inject_at + 1s;
    while (!llm_finished.load(std::memory_order_acquire)
           && steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    while (queue.size() > 0 && steady_clock::now() < cancel_deadline) {
        std::this_thread::sleep_for(5ms);
    }

    // Compute time-to-cancel as the gap between inject and the
    // PlaybackEngine's first post-cancel silent buffer (drained by the
    // tts metrics thread — but in the demo we read it directly).
    double time_to_cancel_ms = engine.consume_pending_barge_in_latency_ms();
    if (time_to_cancel_ms < 0.0) {
        // Engine was already silent (no chunks pending) — fall back to
        // wall-clock since publish.
        time_to_cancel_ms =
            duration<double, std::milli>(steady_clock::now() - inject_at).count();
    }
    const auto sentences_dropped =
        sentences_emitted.load(std::memory_order_relaxed)
        > sentences_played.load(std::memory_order_relaxed)
        ? sentences_emitted.load(std::memory_order_relaxed)
              - sentences_played.load(std::memory_order_relaxed)
        : 0U;
    const char* outcome = llm_cancelled.load() ? "interrupted" : "completed";

    manager.stop();
    bridge.stop();
    engine.stop();
    sub_started->stop();
    sub_sentence->stop();
    sub_emitted->stop();
    sub_pf->stop();
    sub_finished->stop();
    fsm.stop();
    bus.shutdown();
    std::filesystem::remove(cfg.memory.db_path);

    std::printf(
        "\ndemo[bargein] done: time_to_cancel_ms=%.1f sentences_played=%zu "
        "sentences_dropped=%zu outcome=%s (M7 §6 persistence policy)\n",
        time_to_cancel_ms,
        sentences_played.load(std::memory_order_relaxed),
        sentences_dropped, outcome);

    if (!llm_cancelled.load()) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: LlmFinished was not cancelled — cascade did not fire.\n");
        return EXIT_FAILURE;
    }
    if (time_to_cancel_ms > 400.0) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: time_to_cancel_ms=%.1f exceeded the 400 ms "
            "M7 acceptance gate (P95).\n", time_to_cancel_ms);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
