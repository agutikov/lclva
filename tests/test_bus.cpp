#include "event/bus.hpp"
#include "event/event.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using acva::event::EventBus;
using acva::event::SubscribeOptions;
using acva::event::OverflowPolicy;

namespace ev = acva::event;

namespace {

template <class Fn>
bool wait_for(std::chrono::milliseconds timeout, Fn pred) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

} // namespace

TEST_CASE("bus: typed subscription only sees its event type") {
    EventBus bus;
    std::atomic<int> speech_count{0};
    std::atomic<int> llm_count{0};

    SubscribeOptions opts;
    opts.name = "test.speech";
    auto s1 = bus.subscribe<ev::SpeechStarted>(opts,
        [&](const ev::SpeechStarted&) { speech_count.fetch_add(1); });

    opts.name = "test.llm";
    auto s2 = bus.subscribe<ev::LlmStarted>(opts,
        [&](const ev::LlmStarted&) { llm_count.fetch_add(1); });

    bus.publish(ev::SpeechStarted{ .turn = 1 });
    bus.publish(ev::SpeechStarted{ .turn = 2 });
    bus.publish(ev::LlmStarted{ .turn = 1 });

    REQUIRE(wait_for(500ms, [&] { return speech_count == 2 && llm_count == 1; }));
}

TEST_CASE("bus: subscribe_all sees every event") {
    EventBus bus;
    std::atomic<int> total{0};

    SubscribeOptions opts;
    opts.name = "test.all";
    auto s = bus.subscribe_all(opts, [&](const ev::Event&) { total.fetch_add(1); });

    bus.publish(ev::SpeechStarted{ .turn = 1 });
    bus.publish(ev::LlmStarted{ .turn = 1 });
    bus.publish(ev::LlmFinished{ .turn = 1 });

    REQUIRE(wait_for(500ms, [&] { return total == 3; }));
}

TEST_CASE("bus: shutdown joins subscriber workers") {
    EventBus bus;
    std::atomic<int> n{0};
    SubscribeOptions opts;
    opts.name = "test.shutdown";
    auto s = bus.subscribe<ev::SpeechStarted>(opts,
        [&](const ev::SpeechStarted&) { n.fetch_add(1); });
    bus.publish(ev::SpeechStarted{ .turn = 1 });
    bus.publish(ev::SpeechStarted{ .turn = 2 });

    REQUIRE(wait_for(500ms, [&] { return n == 2; }));
    bus.shutdown();
    // After shutdown: publishing is a no-op for delivery.
    bus.publish(ev::SpeechStarted{ .turn = 3 });
    std::this_thread::sleep_for(50ms);
    CHECK(n == 2);
}

TEST_CASE("bus: small queue + DropOldest counts drops") {
    EventBus bus;
    std::atomic<int> seen{0};

    SubscribeOptions opts;
    opts.name = "test.tiny";
    opts.queue_capacity = 2;
    opts.policy = OverflowPolicy::DropOldest;

    // Block the handler so the queue fills up.
    std::atomic<bool> gate{false};
    auto sub = bus.subscribe<ev::LlmToken>(opts, [&](const ev::LlmToken&) {
        while (!gate.load()) std::this_thread::sleep_for(1ms);
        seen.fetch_add(1);
    });

    for (int i = 0; i < 50; ++i) {
        bus.publish(ev::LlmToken{ .turn = 1, .token = "x" });
    }
    // Queue capacity is 2 → many drops.
    auto& q = sub->queue();
    CHECK(q.drops() > 0);
    gate.store(true);
}
