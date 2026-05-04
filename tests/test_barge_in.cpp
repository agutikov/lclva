#include "config/config.hpp"
#include "dialogue/barge_in.hpp"
#include "dialogue/fsm.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace ev  = acva::event;
namespace dlg = acva::dialogue;
namespace cfg = acva::config;

namespace {

// Wait until predicate is true or timeout. Returns final predicate value.
template <class P>
bool wait_for(std::chrono::milliseconds timeout, P p) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return p();
}

// Drive a real Fsm into Speaking state and return the active turn id.
ev::TurnId drive_to_speaking(ev::EventBus& bus, dlg::Fsm& fsm) {
    bus.publish(ev::SpeechStarted{ .turn = 0 });
    REQUIRE(wait_for(200ms, [&]{
        return fsm.snapshot().active_turn != ev::kNoTurn;
    }));
    const auto turn = fsm.snapshot().active_turn;
    bus.publish(ev::SpeechEnded{ .turn = turn });
    bus.publish(ev::FinalTranscript{
        .turn = turn, .text = "ping", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    bus.publish(ev::LlmSentence{
        .turn = turn, .seq = 0, .text = "Reply.", .lang = "en",
    });
    REQUIRE(wait_for(200ms, [&]{
        return fsm.snapshot().state == dlg::State::Speaking;
    }));
    return turn;
}

// Capture UserInterrupted events on a side subscription.
struct InterruptionCollector {
    explicit InterruptionCollector(ev::EventBus& bus) {
        sub_ = bus.subscribe<ev::UserInterrupted>({},
            [this](const ev::UserInterrupted& e) {
                std::lock_guard lk(mu_);
                events_.push_back(e);
            });
    }
    [[nodiscard]] std::size_t count() const {
        std::lock_guard lk(mu_);
        return events_.size();
    }
    [[nodiscard]] ev::TurnId last_turn() const {
        std::lock_guard lk(mu_);
        return events_.empty() ? ev::kNoTurn : events_.back().turn;
    }
private:
    mutable std::mutex mu_;
    std::vector<ev::UserInterrupted> events_;
    ev::SubscriptionHandle sub_;
};

cfg::BargeInConfig default_cfg(bool require_aec = false) {
    cfg::BargeInConfig c;
    c.enabled = true;
    c.require_aec_converged = require_aec;
    c.cool_down_after_turn_ms = 0;     // disable for tests
    c.min_real_utterance_chars = 3;
    return c;
}

} // namespace

TEST_CASE("barge_in: fires UserInterrupted when speaking and AEC not required") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    InterruptionCollector collector(bus);
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, default_cfg());
    detector.start();

    const auto turn = drive_to_speaking(bus, fsm);

    // Now publish a SpeechStarted while Speaking — should fire.
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    REQUIRE(wait_for(200ms, [&]{ return collector.count() >= 1; }));
    CHECK(collector.last_turn() == turn);
    CHECK(detector.fires_total() == 1);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: ignores SpeechStarted when not Speaking") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    InterruptionCollector collector(bus);
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, default_cfg());
    detector.start();

    // FSM is Listening — SpeechStarted should drive normal turn start, not
    // fire barge-in.
    bus.publish(ev::SpeechStarted{ .turn = 0 });
    REQUIRE(wait_for(200ms, [&]{
        return fsm.snapshot().state == dlg::State::UserSpeaking;
    }));

    // Give the detector a chance to (incorrectly) fire.
    std::this_thread::sleep_for(50ms);
    CHECK(collector.count() == 0);
    CHECK(detector.fires_total() == 0);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: cooldown suppresses fires within window") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    auto c = default_cfg();
    c.cool_down_after_turn_ms = 200;       // 200 ms quiet window
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, c);
    detector.start();

    drive_to_speaking(bus, fsm);

    // Within the cooldown — should NOT fire.
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    REQUIRE(wait_for(200ms, [&]{
        return detector.suppressed_cooldown() >= 1;
    }));
    CHECK(detector.fires_total() == 0);
    CHECK(detector.suppressed_total() >= 1);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: AEC required + null Apm suppresses") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    auto c = default_cfg(/*require_aec=*/true);
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, c);
    detector.start();

    drive_to_speaking(bus, fsm);
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    REQUIRE(wait_for(200ms, [&]{ return detector.suppressed_aec() >= 1; }));
    CHECK(detector.fires_total() == 0);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: only fires once per turn") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    InterruptionCollector collector(bus);
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, default_cfg());
    detector.start();

    drive_to_speaking(bus, fsm);

    // Publish three rapid SpeechStarted events for the same speaking turn.
    for (int i = 0; i < 3; ++i) {
        bus.publish(ev::SpeechStarted{ .turn = 0 });
    }
    REQUIRE(wait_for(200ms, [&]{ return detector.fires_total() >= 1; }));
    // Allow time for the duplicates to be (correctly) ignored.
    std::this_thread::sleep_for(50ms);
    CHECK(detector.fires_total() == 1);
    CHECK(collector.count() == 1);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: on_fired callback receives turn + timestamp") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    std::atomic<ev::TurnId> got_turn{ev::kNoTurn};
    std::atomic<bool> got_ts{false};

    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, default_cfg());
    detector.set_on_fired(
        [&](ev::TurnId turn, std::chrono::steady_clock::time_point ts) {
            got_turn.store(turn, std::memory_order_release);
            got_ts.store(ts != std::chrono::steady_clock::time_point{},
                          std::memory_order_release);
        });
    detector.start();

    const auto turn = drive_to_speaking(bus, fsm);
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    REQUIRE(wait_for(200ms, [&]{
        return got_turn.load(std::memory_order_acquire) != ev::kNoTurn;
    }));
    CHECK(got_turn.load() == turn);
    CHECK(got_ts.load());

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: disabled config is a no-op") {
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    InterruptionCollector collector(bus);
    auto c = default_cfg();
    c.enabled = false;
    dlg::BargeInDetector detector(bus, fsm, /*apm=*/nullptr, /*system_aec=*/false, c);
    detector.start();

    drive_to_speaking(bus, fsm);
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    std::this_thread::sleep_for(80ms);
    CHECK(detector.fires_total() == 0);
    CHECK(collector.count() == 0);

    detector.stop();
    fsm.stop();
    bus.shutdown();
}

TEST_CASE("barge_in: system AEC mode bypasses in-process APM gate") {
    // M6B Path B regression: with cfg.apm.use_system_aec=true the
    // in-process APM is intentionally a stub. Pre-fix the detector
    // gated on apm_->aec_active() which returns false in that mode,
    // so every SpeechStarted was suppressed and barge-in literally
    // never fired in production. Pin the new behavior: when
    // system_aec_active=true, AEC is treated as always-converged
    // even with require_aec_converged=true and a null/stub Apm.
    ev::EventBus bus;
    dlg::TurnFactory turns;
    dlg::Fsm fsm(bus, turns);
    fsm.start();

    InterruptionCollector collector(bus);
    auto c = default_cfg(/*require_aec=*/true);  // strict gate ON
    dlg::BargeInDetector detector(bus, fsm,
                                   /*apm=*/nullptr,
                                   /*system_aec=*/true,    // ← the fix
                                   c);
    detector.start();

    const auto turn = drive_to_speaking(bus, fsm);
    bus.publish(ev::SpeechStarted{ .turn = 0 });

    REQUIRE(wait_for(200ms, [&]{ return collector.count() >= 1; }));
    CHECK(collector.last_turn() == turn);
    CHECK(detector.fires_total() == 1);
    CHECK(detector.suppressed_aec() == 0);  // not gated on AEC

    detector.stop();
    fsm.stop();
    bus.shutdown();
}
