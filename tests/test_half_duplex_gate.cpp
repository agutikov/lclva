// Unit tests for HalfDuplexGate — the M5 mic-gating helper for the
// speakers-without-AEC fallback (see plans/milestones/m5_streaming_stt.md
// "Half-duplex" section). The gate is templated on a Clock so we can
// inject a virtual one and exercise hangover timing without sleeping.

#include "audio/half_duplex_gate.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Drop-in std::chrono-shaped clock backed by an atomic counter we can
// step deterministically.
struct ManualClock {
    using rep        = std::int64_t;
    using period     = std::nano;
    using duration   = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<ManualClock, duration>;

    static inline std::atomic<rep> now_ns_{0};

    static time_point now() noexcept {
        return time_point{duration{now_ns_.load(std::memory_order_acquire)}};
    }
    static void advance(std::chrono::milliseconds delta) noexcept {
        now_ns_.fetch_add(
            std::chrono::duration_cast<duration>(delta).count(),
            std::memory_order_acq_rel);
    }
    static void reset() noexcept { now_ns_.store(0, std::memory_order_release); }
};

using Gate = acva::audio::BasicHalfDuplexGate<ManualClock>;

} // namespace

TEST_CASE("HalfDuplexGate: defaults to not dropping") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    CHECK_FALSE(g.should_drop_now());
    CHECK_FALSE(g.speaking());
}

TEST_CASE("HalfDuplexGate: drops while speaking is active") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    g.set_speaking(true);
    CHECK(g.speaking());
    CHECK(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: hangover keeps dropping after speaking ends") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    g.set_speaking(true);
    ManualClock::advance(std::chrono::milliseconds(50));
    g.set_speaking(false);
    CHECK_FALSE(g.speaking());
    // Right at the falling edge — gate should still drop (within hangover).
    CHECK(g.should_drop_now());
    ManualClock::advance(std::chrono::milliseconds(100));  // 100ms into hangover
    CHECK(g.should_drop_now());
    ManualClock::advance(std::chrono::milliseconds(99));   // 199ms into hangover (< 200)
    CHECK(g.should_drop_now());
    ManualClock::advance(std::chrono::milliseconds(2));    // 201ms into hangover (> 200)
    CHECK_FALSE(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: zero hangover means immediate unmute on falling edge") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(0)};
    g.set_speaking(true);
    CHECK(g.should_drop_now());
    g.set_speaking(false);
    // Hangover < 1 tick: immediately not-dropping.
    CHECK_FALSE(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: re-entering speaking cancels hangover countdown") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    g.set_speaking(true);
    g.set_speaking(false);
    ManualClock::advance(std::chrono::milliseconds(150)); // mid-hangover
    CHECK(g.should_drop_now());

    // FSM goes back to Speaking before hangover expired (e.g. next sentence).
    g.set_speaking(true);
    ManualClock::advance(std::chrono::milliseconds(500));
    CHECK(g.should_drop_now()); // because speaking_ is true again

    g.set_speaking(false);
    // New hangover anchored at *this* falling edge; older one is moot.
    ManualClock::advance(std::chrono::milliseconds(199));
    CHECK(g.should_drop_now());
    ManualClock::advance(std::chrono::milliseconds(2));
    CHECK_FALSE(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: idempotent set_speaking(true) doesn't disturb hangover anchor") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    g.set_speaking(true);
    g.set_speaking(true);  // idempotent rising-edge
    CHECK(g.should_drop_now());
    g.set_speaking(false);
    ManualClock::advance(std::chrono::milliseconds(199));
    CHECK(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: idempotent set_speaking(false) doesn't reset hangover") {
    ManualClock::reset();
    Gate g{std::chrono::milliseconds(200)};
    g.set_speaking(true);
    g.set_speaking(false);
    ManualClock::advance(std::chrono::milliseconds(150));
    g.set_speaking(false); // already false — must NOT push the anchor forward
    ManualClock::advance(std::chrono::milliseconds(60));
    // Total elapsed since first falling edge: 210ms; hangover is 200ms.
    // If the second set_speaking(false) had reset the anchor, gate
    // would still be dropping. It must not.
    CHECK_FALSE(g.should_drop_now());
}

TEST_CASE("HalfDuplexGate: real steady_clock variant constructs and reports") {
    acva::audio::HalfDuplexGate g{std::chrono::milliseconds(50)};
    CHECK(g.hangover() == std::chrono::milliseconds(50));
    CHECK_FALSE(g.should_drop_now());
    g.set_speaking(true);
    CHECK(g.should_drop_now());
    g.set_speaking(false);
    // Briefly within hangover.
    CHECK(g.should_drop_now());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_FALSE(g.should_drop_now());
}
