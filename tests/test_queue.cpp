#include "event/queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using acva::event::BoundedQueue;
using acva::event::OverflowPolicy;

TEST_CASE("queue: push/pop ordering") {
    BoundedQueue<int> q(8, OverflowPolicy::Block);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    auto a = q.try_pop();
    auto b = q.try_pop();
    auto c = q.try_pop();
    REQUIRE(a.has_value()); CHECK(*a == 1);
    REQUIRE(b.has_value()); CHECK(*b == 2);
    REQUIRE(c.has_value()); CHECK(*c == 3);
    CHECK_FALSE(q.try_pop().has_value());
    CHECK(q.pushes() == 3);
    CHECK(q.pops() == 3);
    CHECK(q.drops() == 0);
}

TEST_CASE("queue: DropOldest evicts FIFO") {
    BoundedQueue<int> q(3, OverflowPolicy::DropOldest);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    REQUIRE(q.push(3));
    REQUIRE(q.push(4)); // 1 is dropped
    REQUIRE(q.push(5)); // 2 is dropped
    CHECK(q.size() == 3);
    CHECK(q.drops() == 2);
    auto a = q.try_pop(); CHECK(*a == 3);
    auto b = q.try_pop(); CHECK(*b == 4);
    auto c = q.try_pop(); CHECK(*c == 5);
}

TEST_CASE("queue: DropNewest rejects new on full") {
    BoundedQueue<int> q(2, OverflowPolicy::DropNewest);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));
    CHECK_FALSE(q.push(3));
    CHECK_FALSE(q.push(4));
    CHECK(q.drops() == 2);
    auto a = q.try_pop(); CHECK(*a == 1);
    auto b = q.try_pop(); CHECK(*b == 2);
}

TEST_CASE("queue: Block policy actually blocks") {
    BoundedQueue<int> q(2, OverflowPolicy::Block);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        REQUIRE(q.push(3));
        producer_done.store(true);
    });

    // Producer should still be blocked.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(producer_done.load());

    auto consumed = q.try_pop();
    REQUIRE(consumed.has_value());
    CHECK(*consumed == 1);

    producer.join();
    CHECK(producer_done.load());
    CHECK(q.size() == 2);
}

TEST_CASE("queue: close wakes blocked producers (no concurrent consumer)") {
    BoundedQueue<int> q(2, OverflowPolicy::Block);
    REQUIRE(q.push(1));
    REQUIRE(q.push(2));

    std::atomic<bool> producer_returned{false};
    std::atomic<bool> producer_result{true};
    std::thread blocked_producer([&] {
        // No consumer is running, so this blocks until close() unblocks it.
        bool r = q.push(3);
        producer_result.store(r);
        producer_returned.store(true);
    });

    // Producer should be blocked (queue full, no consumer).
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE_FALSE(producer_returned.load());

    q.close();
    blocked_producer.join();

    CHECK(producer_returned.load());
    CHECK_FALSE(producer_result.load()); // close-while-blocked → push returns false
    CHECK(q.closed());
    CHECK_FALSE(q.push(99));
}

TEST_CASE("queue: close lets pop drain remaining items then return nullopt") {
    BoundedQueue<int> q(4, OverflowPolicy::Block);
    REQUIRE(q.push(10));
    REQUIRE(q.push(20));
    q.close();

    auto a = q.pop(); REQUIRE(a.has_value()); CHECK(*a == 10);
    auto b = q.pop(); REQUIRE(b.has_value()); CHECK(*b == 20);
    CHECK_FALSE(q.pop().has_value()); // closed-and-empty
}

TEST_CASE("queue: pop_until honors deadline") {
    BoundedQueue<int> q(8, OverflowPolicy::Block);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    auto r = q.pop_until(deadline);
    CHECK_FALSE(r.has_value());
    CHECK(std::chrono::steady_clock::now() >= deadline);
}
