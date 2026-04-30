#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
namespace ev = acva::event;
namespace dl = acva::dialogue;

namespace {

bool wait_state(const dl::Fsm& fsm, dl::State target, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fsm.snapshot().state == target) return true;
        std::this_thread::sleep_for(1ms);
    }
    return fsm.snapshot().state == target;
}

template <class Pred>
bool wait_for_pred(const dl::Fsm& fsm, std::chrono::milliseconds timeout, Pred p) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p(fsm.snapshot())) return true;
        std::this_thread::sleep_for(1ms);
    }
    return p(fsm.snapshot());
}

// Drive the FSM through a full turn. The FSM mints its own turn id on
// SpeechStarted; subsequent events must carry that id to be accepted, so we
// read the active turn back from the FSM snapshot before publishing the
// rest of the chain.
void run_full_turn(ev::EventBus& bus, dl::Fsm& fsm, std::uint32_t sentences) {
    bus.publish(ev::SpeechStarted{ .turn = 0 });
    auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (fsm.snapshot().active_turn == acva::dialogue::kNoTurn
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto turn = fsm.snapshot().active_turn;
    REQUIRE(turn != acva::dialogue::kNoTurn);
    bus.publish(ev::SpeechEnded{ .turn = turn });
    bus.publish(ev::FinalTranscript{ .turn = turn, .text = "hello", .lang = "en" });
    for (std::uint32_t s = 1; s <= sentences; ++s) {
        bus.publish(ev::LlmSentence{ .turn = turn, .seq = s, .text = "...", .lang = "en" });
        bus.publish(ev::PlaybackFinished{ .turn = turn, .seq = s });
    }
    bus.publish(ev::LlmFinished{ .turn = turn });
}

} // namespace

TEST_CASE("fsm: starts in Listening after start()") {
    ev::EventBus bus;
    dl::TurnFactory turns;
    dl::Fsm fsm(bus, turns);
    fsm.start();
    REQUIRE(wait_state(fsm, dl::State::Listening, 200ms));
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("fsm: full turn drives to Completed and back to Listening") {
    ev::EventBus bus;
    dl::TurnFactory turns;
    dl::Fsm fsm(bus, turns);
    fsm.start();
    REQUIRE(wait_state(fsm, dl::State::Listening, 200ms));

    run_full_turn(bus, fsm, /*sentences=*/3);

    REQUIRE(wait_for_pred(fsm, 1500ms,
        [](const dl::FsmSnapshot& s) { return s.turns_completed == 1; }));
    auto snap = fsm.snapshot();
    CHECK(snap.state == dl::State::Listening);
    CHECK(snap.turns_completed == 1);
    CHECK(snap.turns_interrupted == 0);
    CHECK(snap.turns_discarded == 0);
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("fsm: barge-in before any sentence plays → Discarded") {
    ev::EventBus bus;
    dl::TurnFactory turns;
    dl::Fsm fsm(bus, turns);

    std::atomic<int> outcomes{0};
    fsm.set_turn_outcome_observer([&](const char* o) {
        // We expect "discarded" first.
        if (std::string{o} == "discarded") outcomes.fetch_add(1);
    });
    fsm.start();
    REQUIRE(wait_state(fsm, dl::State::Listening, 200ms));

    bus.publish(ev::SpeechStarted{ .turn = 0 });
    REQUIRE(wait_state(fsm, dl::State::UserSpeaking, 200ms));
    bus.publish(ev::SpeechEnded{ .turn = 0 });
    REQUIRE(wait_state(fsm, dl::State::Transcribing, 200ms));
    bus.publish(ev::FinalTranscript{ .turn = 0, .text = "x", .lang = "en" });
    REQUIRE(wait_state(fsm, dl::State::Thinking, 200ms));

    // No sentence has played yet. Barge-in here → Discarded.
    auto active = fsm.snapshot().active_turn;
    bus.publish(ev::UserInterrupted{ .turn = active });

    REQUIRE(wait_state(fsm, dl::State::Listening, 500ms));
    auto snap = fsm.snapshot();
    CHECK(snap.turns_discarded == 1);
    CHECK(snap.turns_interrupted == 0);
    CHECK(snap.turns_completed == 0);
    CHECK(outcomes.load() == 1);

    fsm.stop();
    bus.shutdown();
}

TEST_CASE("fsm: barge-in after a sentence plays → Interrupted") {
    ev::EventBus bus;
    dl::TurnFactory turns;
    dl::Fsm fsm(bus, turns);
    fsm.start();
    REQUIRE(wait_state(fsm, dl::State::Listening, 200ms));

    bus.publish(ev::SpeechStarted{ .turn = 0 });
    bus.publish(ev::SpeechEnded{ .turn = 0 });
    bus.publish(ev::FinalTranscript{ .turn = 0, .text = "x", .lang = "en" });
    REQUIRE(wait_state(fsm, dl::State::Thinking, 200ms));

    auto active = fsm.snapshot().active_turn;
    bus.publish(ev::LlmSentence{ .turn = active, .seq = 1, .text = "...", .lang = "en" });
    REQUIRE(wait_state(fsm, dl::State::Speaking, 200ms));
    bus.publish(ev::PlaybackFinished{ .turn = active, .seq = 1 });

    // Wait until the FSM has observed the playback.
    auto deadline = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < deadline
           && fsm.snapshot().sentences_played < 1) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(fsm.snapshot().sentences_played >= 1);

    bus.publish(ev::UserInterrupted{ .turn = active });
    REQUIRE(wait_state(fsm, dl::State::Listening, 500ms));

    auto snap = fsm.snapshot();
    CHECK(snap.turns_interrupted == 1);
    CHECK(snap.turns_discarded == 0);
    CHECK(snap.turns_completed == 0);

    fsm.stop();
    bus.shutdown();
}

TEST_CASE("fsm: stale playback events from old turns are ignored") {
    ev::EventBus bus;
    dl::TurnFactory turns;
    dl::Fsm fsm(bus, turns);
    fsm.start();
    REQUIRE(wait_state(fsm, dl::State::Listening, 200ms));

    // Begin turn 1.
    bus.publish(ev::SpeechStarted{ .turn = 0 });
    bus.publish(ev::SpeechEnded{ .turn = 0 });
    bus.publish(ev::FinalTranscript{ .turn = 0, .text = "x", .lang = "en" });
    REQUIRE(wait_state(fsm, dl::State::Thinking, 200ms));
    auto turn1 = fsm.snapshot().active_turn;
    REQUIRE(turn1 != acva::dialogue::kNoTurn);

    // A late playback chunk for a non-existent old turn must be ignored.
    bus.publish(ev::PlaybackFinished{ .turn = turn1 + 99, .seq = 99 });
    std::this_thread::sleep_for(50ms);
    CHECK(fsm.snapshot().sentences_played == 0);

    fsm.stop();
    bus.shutdown();
}
