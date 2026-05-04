#include "demos/demo.hpp"

#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "pipeline/fake_driver.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace acva::demos {

int run_fsm(const config::Config& cfg, std::span<const std::string> /*args*/) {
    using namespace std::chrono;

    constexpr int kTurnsToObserve = 3;

    event::EventBus bus;
    dialogue::TurnFactory turns;
    dialogue::Fsm fsm(bus, turns);

    // Capture every "interesting" outcome; we rely on the FSM's own
    // observer hook for that rather than re-deriving from raw events.
    std::atomic<int> completed{0};
    std::atomic<int> interrupted{0};
    std::atomic<int> discarded{0};
    fsm.set_turn_outcome_observer([&](const char* o) {
        const std::string_view s{o};
        if      (s == "completed")   completed.fetch_add(1);
        else if (s == "interrupted") interrupted.fetch_add(1);
        else if (s == "discarded")   discarded.fetch_add(1);
    });
    fsm.start();

    pipeline::FakeDriverOptions opts;
    opts.sentences_per_turn      = cfg.pipeline.fake_sentences_per_turn;
    opts.idle_between_turns      = std::chrono::milliseconds(
                                       cfg.pipeline.fake_idle_between_turns_ms);
    opts.barge_in_probability    = cfg.pipeline.fake_barge_in_probability;
    pipeline::FakeDriver driver(bus, opts);

    std::printf(
        "demo[fsm] fake driver running for %d turn%s "
        "(sentences/turn=%u, idle=%ums, barge-in p=%.2f)\n",
        kTurnsToObserve, kTurnsToObserve == 1 ? "" : "s",
        opts.sentences_per_turn,
        cfg.pipeline.fake_idle_between_turns_ms,
        opts.barge_in_probability);

    const auto t0 = steady_clock::now();
    driver.start();

    // Wait for kTurnsToObserve outcomes (completed + interrupted +
    // discarded), with a generous timeout — each turn takes
    // user_speech_duration + stt + llm_first + sentences*per_sentence
    // + tts + playback ≈ ~3 s by default.
    const auto deadline = t0 + std::chrono::milliseconds(
        cfg.pipeline.fake_idle_between_turns_ms * 4u
        + 6000u);
    while (completed.load() + interrupted.load() + discarded.load()
           < kTurnsToObserve
           && steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    driver.stop();
    fsm.stop();
    bus.shutdown();

    const auto snap = fsm.snapshot();
    const auto elapsed_ms =
        duration<double, std::milli>(steady_clock::now() - t0).count();

    std::printf(
        "demo[fsm] done in %.1fms: completed=%llu interrupted=%llu "
        "discarded=%llu (driver emitted %llu turns)\n",
        elapsed_ms,
        static_cast<unsigned long long>(snap.turns_completed),
        static_cast<unsigned long long>(snap.turns_interrupted),
        static_cast<unsigned long long>(snap.turns_discarded),
        static_cast<unsigned long long>(driver.turns_emitted()));

    if (completed.load() + interrupted.load() + discarded.load() == 0) {
        std::fprintf(stderr,
            "demo[fsm] FAIL: no turn outcomes observed in %.1f ms — the FSM "
            "may not be receiving fake-driver events. Check the bus wiring.\n",
            elapsed_ms);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
