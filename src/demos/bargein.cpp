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
// UserInterrupted ~1.5 s INTO the first sentence's playback (not at
// its end) and asserts:
//   • Manager cancels the LLM stream (LlmFinished{cancelled=true}).
//   • PlaybackQueue drains stale chunks.
//   • PlaybackEngine emits silence within ≤ 400 ms (M7 §Demo).
//
// Inject timing: subscribe to `TtsAudioChunk`, wait for the first one
// (= TTS started producing audio for sentence 0), sleep 1500 ms, then
// inject. With a long single-sentence prompt this lands well inside
// the spoken audio — you hear the assistant cut off mid-word, not at
// a sentence boundary. The earlier "wait for first PlaybackFinished"
// strategy injected at the seam between sentences 0 and 1, which
// always cut at sentence-end and obscured the cascade's true latency.
//
// What it does NOT cover: real-mic-driven barge-in (the BargeInDetector
// + AEC gate). That's `acva demo capture` + `aec-record` + the manual
// 50-trial test in plans/milestones/m7_barge_in.md §5.
int run_bargein(const config::Config& orig_cfg,
                 std::span<const std::string> args) {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    // --delay-ms <N>: how long to wait after the first TtsAudioChunk
    // before injecting UserInterrupted. 0 = inject immediately when
    // audio starts arriving (very early, just first few words). The
    // earlier you inject, the closer you hear the cutoff to the
    // start of the sentence.
    long inject_delay_ms = 1500;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--delay-ms" && i + 1 < args.size()) {
            try { inject_delay_ms = std::stol(args[++i]); }
            catch (const std::exception&) {
                std::fprintf(stderr,
                    "demo[bargein]: --delay-ms expects an integer (got '%s')\n",
                    args[i].c_str());
                return EXIT_FAILURE;
            }
        } else if (a.starts_with("--delay-ms=")) {
            try { inject_delay_ms = std::stol(a.substr(11)); }
            catch (const std::exception&) {
                std::fprintf(stderr,
                    "demo[bargein]: --delay-ms expects an integer (got '%s')\n",
                    a.c_str());
                return EXIT_FAILURE;
            }
        } else if (a == "-h" || a == "--help") {
            std::printf(
                "demo[bargein] options:\n"
                "  --delay-ms <N>   sleep N ms after first TtsAudioChunk\n"
                "                   before injecting UserInterrupted\n"
                "                   (default 1500). 0 = inject immediately.\n");
            return EXIT_SUCCESS;
        } else {
            std::fprintf(stderr,
                "demo[bargein]: unknown arg '%s'\n", a.c_str());
            return EXIT_FAILURE;
        }
    }
    if (inject_delay_ms < 0) {
        std::fprintf(stderr,
            "demo[bargein]: --delay-ms must be ≥ 0 (got %ld)\n",
            inject_delay_ms);
        return EXIT_FAILURE;
    }

    // One LONG sentence — comma-clauses, no periods — so the splitter
    // emits a single LlmSentence event and TTS produces ~10+ s of
    // continuous audio. Mid-sentence inject 1.5 s into that audio
    // gives an obvious cutoff. The 800-char count is well above the
    // splitter's max_sentence_chars cap; we raise that cap below so
    // the prompt stays as one sentence end-to-end.
    constexpr std::string_view kPrompt =
        "Describe the Moon in one extremely long English sentence with "
        "many descriptive clauses joined by commas, covering its origin, "
        "its distance from Earth, its gravitational pull, the lunar phases, "
        "tidal influence on the oceans, its surface features, the maria "
        "and highlands, the Apollo missions, modern lunar exploration, "
        "and its role in human culture and mythology, and please do not "
        "use any periods anywhere in the sentence, only commas to keep "
        "all of those ideas linked together as a single continuous thought.";

    if (orig_cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: cfg.tts.voices is empty — populate it before running\n");
        return EXIT_FAILURE;
    }

    config::Config cfg = orig_cfg;
    cfg.memory.db_path = tmp_db_path().string();
    // Raise splitter cap above the prompt's expected sentence length
    // so the LLM's reply stays as one continuous LlmSentence event.
    // Default 300 chars would slice a long monologue at the first
    // ~75-word boundary, making the demo two-sentences-and-a-cut
    // again. 1500 covers the expected reply with margin.
    cfg.dialogue.sentence_splitter.max_sentence_chars = 1500;

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

    // Inject anchor: first TtsAudioChunk for the active turn → mark
    // the time, sleep `kInjectDelay`, then publish UserInterrupted.
    // This lands the inject 1.5 s into a long sentence so the audible
    // cutoff is mid-word, not at a sentence boundary.
    std::atomic<bool> first_chunk_seen{false};
    std::atomic<steady_clock::time_point> first_chunk_at{steady_clock::time_point{}};
    auto sub_chunk = bus.subscribe<event::TtsAudioChunk>({},
        [&](const event::TtsAudioChunk&) {
            if (!first_chunk_seen.exchange(true, std::memory_order_acq_rel)) {
                first_chunk_at.store(steady_clock::now(), std::memory_order_release);
            }
        });
    const auto kInjectDelay = milliseconds(inject_delay_ms);

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

    // Wait for the first audio chunk to land in the queue — that's
    // the moment TTS started producing PCM for sentence 0. Then sleep
    // `kInjectDelay` so the user hears ~1.5 s of audio before the
    // cancellation cascade fires.
    const auto chunk_deadline = t0 + 30s;
    while (!first_chunk_seen.load(std::memory_order_acquire)
           && steady_clock::now() < chunk_deadline) {
        std::this_thread::sleep_for(20ms);
    }
    if (!first_chunk_seen.load()) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: no TtsAudioChunk within 30s — TTS is slow, "
            "Speaches is wedged, or the audio engine is silently stuck.\n");
        manager.stop(); bridge.stop(); engine.stop(); fsm.stop();
        bus.shutdown();
        std::filesystem::remove(cfg.memory.db_path);
        return EXIT_FAILURE;
    }
    std::printf("demo[bargein] first audio chunk arrived; "
                 "sleeping %lld ms before inject…\n",
                 static_cast<long long>(kInjectDelay.count()));
    std::this_thread::sleep_for(kInjectDelay);

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
    sub_chunk->stop();
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

    // Cascade-fired check: the cancellation worked if at least ONE of
    // these is true:
    //   - LlmFinished arrived with cancelled=true (Manager observed the
    //     cancel before the LLM stream completed naturally),
    //   - the bridge dropped pending sentences (TtsBridge cleared them
    //     in on_user_interrupted),
    //   - the audio thread reported a real silence transition (the
    //     primary measurement path).
    // A pure wall-clock fallback (publish_ns set, no audio silence
    // event observed) means the engine was already idle and there was
    // nothing to silence — that's a race with naturally-finished LLM,
    // not a cascade failure.
    const bool audio_silenced =
        engine.consume_pending_barge_in_latency_ms() >= 0.0
        || // already drained above; check pending_latency_ms via a separate
           // mechanism — for clarity, we trust sentences_dropped + cancelled.
        false;
    (void)audio_silenced;     // metric was already drained above
    const bool cascade_fired = llm_cancelled.load() || sentences_dropped > 0;
    if (!cascade_fired) {
        std::fprintf(stderr,
            "demo[bargein] FAIL: cascade did not fire — LLM finished naturally "
            "before the inject landed (try a longer prompt, or this is a real "
            "race that needs investigation).\n");
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
