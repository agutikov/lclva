#include "supervisor/keep_alive.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using acva::supervisor::KeepAlive;

TEST_CASE("KeepAlive: fires on_tick when should_fire returns true") {
    std::atomic<int> ticks{0};
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::milliseconds(5),
        .should_fire = []{ return true; },
        .on_tick     = [&]{ ticks.fetch_add(1); },
        .on_fired    = {},
        .on_skipped  = {},
    });
    ka.start();
    // Wait long enough for ~3 ticks. The first tick fires after the
    // first interval, not at start, so 30 ms is comfortable.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ka.stop();
    CHECK(ticks.load() >= 2);
    CHECK(ka.fired() >= 2);
    CHECK(ka.skipped() == 0);
}

TEST_CASE("KeepAlive: skips on_tick when should_fire returns false") {
    std::atomic<int> ticks{0};
    std::atomic<int> skips{0};
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::milliseconds(5),
        .should_fire = []{ return false; },
        .on_tick     = [&]{ ticks.fetch_add(1); },
        .on_fired    = {},
        .on_skipped  = [&]{ skips.fetch_add(1); },
    });
    ka.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ka.stop();
    CHECK(ticks.load() == 0);
    CHECK(skips.load() >= 2);
    CHECK(ka.fired() == 0);
    CHECK(ka.skipped() >= 2);
}

TEST_CASE("KeepAlive: predicate change flips behaviour mid-flight") {
    std::atomic<bool> active{false};        // false → fire; true → skip
    std::atomic<int> ticks{0};
    std::atomic<int> skips{0};
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::milliseconds(4),
        .should_fire = [&]{ return !active.load(); },
        .on_tick     = [&]{ ticks.fetch_add(1); },
        .on_fired    = {},
        .on_skipped  = [&]{ skips.fetch_add(1); },
    });
    ka.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto ticks_phase1 = ticks.load();
    CHECK(ticks_phase1 >= 2);

    active.store(true);                     // simulate active turn
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto ticks_phase2 = ticks.load();
    CHECK(ticks_phase2 == ticks_phase1);     // no new ticks while active
    CHECK(skips.load() >= 2);

    active.store(false);                    // back to listening
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(ticks.load() > ticks_phase2);
    ka.stop();
}

TEST_CASE("KeepAlive: stop() interrupts a long wait promptly") {
    std::atomic<int> ticks{0};
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::seconds(60),    // never elapses naturally
        .should_fire = []{ return true; },
        .on_tick     = [&]{ ticks.fetch_add(1); },
        .on_fired    = {},
        .on_skipped  = {},
    });
    ka.start();
    const auto t0 = std::chrono::steady_clock::now();
    ka.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(ticks.load() == 0);
    CHECK(elapsed < std::chrono::milliseconds(200));
}

TEST_CASE("KeepAlive: missing callbacks don't start the thread") {
    KeepAlive ka(KeepAlive::Options{});     // no should_fire/on_tick
    ka.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(ka.fired() == 0);
    CHECK(ka.skipped() == 0);
    ka.stop();   // must be a no-op
}

TEST_CASE("KeepAlive: on_tick exception is swallowed and counted as fired") {
    std::atomic<int> attempts{0};
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::milliseconds(5),
        .should_fire = []{ return true; },
        .on_tick     = [&]{
            attempts.fetch_add(1);
            throw std::runtime_error("LLM down");
        },
        .on_fired    = {},
        .on_skipped  = {},
    });
    ka.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ka.stop();
    CHECK(attempts.load() >= 2);
    // Threw, but the loop survived — fired_ counts each completed
    // attempt regardless of whether on_tick raised.
    CHECK(ka.fired() >= 2);
}

TEST_CASE("KeepAlive: stop is idempotent") {
    KeepAlive ka(KeepAlive::Options{
        .interval    = std::chrono::milliseconds(5),
        .should_fire = []{ return true; },
        .on_tick     = []{},
        .on_fired    = {},
        .on_skipped  = {},
    });
    ka.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ka.stop();
    ka.stop();   // must not deadlock or crash
    CHECK(ka.fired() >= 2);
}
