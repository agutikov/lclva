#include "playback/queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>

using acva::dialogue::TurnId;
using acva::event::SequenceNo;
using acva::playback::AudioChunk;
using acva::playback::PlaybackQueue;

namespace {

AudioChunk make_chunk(TurnId turn, SequenceNo seq, std::size_t n_samples = 4) {
    AudioChunk c;
    c.turn = turn;
    c.seq  = seq;
    c.samples.resize(n_samples);
    for (std::size_t i = 0; i < n_samples; ++i) {
        c.samples[i] = static_cast<std::int16_t>(turn * 1000 + seq * 10 + i);
    }
    return c;
}

} // namespace

TEST_CASE("PlaybackQueue: enqueue/dequeue preserves order within a turn") {
    PlaybackQueue q(8);
    CHECK(q.enqueue(make_chunk(7, 0)));
    CHECK(q.enqueue(make_chunk(7, 1)));
    CHECK(q.enqueue(make_chunk(7, 2)));
    CHECK(q.size() == 3);

    auto a = q.dequeue_active(7);
    REQUIRE(a.has_value());
    CHECK(a->turn == 7);
    CHECK(a->seq == 0);

    auto b = q.dequeue_active(7);
    REQUIRE(b.has_value());
    CHECK(b->seq == 1);

    auto c = q.dequeue_active(7);
    REQUIRE(c.has_value());
    CHECK(c->seq == 2);

    CHECK_FALSE(q.dequeue_active(7).has_value());
    CHECK(q.size() == 0);
    CHECK(q.dequeued() == 3);
    CHECK(q.drops() == 0);
}

TEST_CASE("PlaybackQueue: dequeue_active drops stale-turn head and counts them") {
    PlaybackQueue q(8);
    REQUIRE(q.enqueue(make_chunk(5, 0)));   // stale
    REQUIRE(q.enqueue(make_chunk(5, 1)));   // stale
    REQUIRE(q.enqueue(make_chunk(6, 0)));   // active
    REQUIRE(q.enqueue(make_chunk(6, 1)));   // active

    // Active = 6: the two stale 5s are dropped at the head, then the
    // first 6 chunk is returned.
    auto first = q.dequeue_active(6);
    REQUIRE(first.has_value());
    CHECK(first->turn == 6);
    CHECK(first->seq == 0);
    CHECK(q.drops() == 2);

    auto second = q.dequeue_active(6);
    REQUIRE(second.has_value());
    CHECK(second->seq == 1);

    CHECK(q.size() == 0);
}

TEST_CASE("PlaybackQueue: empty queue returns nullopt cleanly") {
    PlaybackQueue q(4);
    CHECK_FALSE(q.dequeue_active(1).has_value());
    CHECK(q.size() == 0);
    CHECK(q.drops() == 0);
}

TEST_CASE("PlaybackQueue: only-stale-chunks queue returns nullopt and drains") {
    PlaybackQueue q(4);
    REQUIRE(q.enqueue(make_chunk(1, 0)));
    REQUIRE(q.enqueue(make_chunk(1, 1)));
    REQUIRE(q.enqueue(make_chunk(1, 2)));

    // Active turn jumped to 99 (barge-in plus a few user utterances).
    // No chunks match → nullopt and the queue empties.
    auto out = q.dequeue_active(99);
    CHECK_FALSE(out.has_value());
    CHECK(q.size() == 0);
    CHECK(q.drops() == 3);
}

TEST_CASE("PlaybackQueue: invalidate_before drops only earlier turns") {
    PlaybackQueue q(8);
    REQUIRE(q.enqueue(make_chunk(3, 0)));
    REQUIRE(q.enqueue(make_chunk(3, 1)));
    REQUIRE(q.enqueue(make_chunk(4, 0)));
    REQUIRE(q.enqueue(make_chunk(5, 0)));

    const auto dropped = q.invalidate_before(4);
    CHECK(dropped == 2);          // both turn=3 chunks
    CHECK(q.size() == 2);         // the turn=4 and turn=5 chunks survive
    CHECK(q.drops() == 2);

    // Subsequent invalidate with the same threshold drops nothing.
    CHECK(q.invalidate_before(4) == 0);

    // Now bump past 5 — drops everything.
    const auto dropped2 = q.invalidate_before(6);
    CHECK(dropped2 == 2);
    CHECK(q.size() == 0);
}

TEST_CASE("PlaybackQueue: invalidate_before preserves order of survivors") {
    PlaybackQueue q(8);
    // Interleaved insertion order — survivors must come out in the
    // same relative order as they went in.
    REQUIRE(q.enqueue(make_chunk(1, 0)));
    REQUIRE(q.enqueue(make_chunk(2, 0)));
    REQUIRE(q.enqueue(make_chunk(1, 1)));   // stale relative to next bump
    REQUIRE(q.enqueue(make_chunk(2, 1)));
    REQUIRE(q.enqueue(make_chunk(2, 2)));

    REQUIRE(q.invalidate_before(2) == 2);   // both turn=1 entries gone

    auto a = q.dequeue_active(2); REQUIRE(a); CHECK(a->seq == 0);
    auto b = q.dequeue_active(2); REQUIRE(b); CHECK(b->seq == 1);
    auto c = q.dequeue_active(2); REQUIRE(c); CHECK(c->seq == 2);
}

TEST_CASE("PlaybackQueue: capacity overflow rejects new chunks") {
    PlaybackQueue q(3);
    CHECK(q.enqueue(make_chunk(1, 0)));
    CHECK(q.enqueue(make_chunk(1, 1)));
    CHECK(q.enqueue(make_chunk(1, 2)));
    // Queue at capacity — fourth enqueue must fail and count.
    CHECK_FALSE(q.enqueue(make_chunk(1, 3)));
    CHECK(q.drops() == 1);
    CHECK(q.size() == 3);
    CHECK(q.enqueued() == 3);

    // Once a chunk is consumed there is room again.
    CHECK(q.dequeue_active(1).has_value());
    CHECK(q.enqueue(make_chunk(1, 4)));
    CHECK(q.size() == 3);
}

TEST_CASE("PlaybackQueue: clear() drops everything") {
    PlaybackQueue q(8);
    for (SequenceNo i = 0; i < 5; ++i) REQUIRE(q.enqueue(make_chunk(1, i)));
    CHECK(q.size() == 5);
    CHECK(q.clear() == 5);
    CHECK(q.size() == 0);
    CHECK(q.drops() == 5);
    CHECK_FALSE(q.dequeue_active(1).has_value());
}

TEST_CASE("PlaybackQueue: counters are monotonic and consistent") {
    PlaybackQueue q(4);
    REQUIRE(q.enqueue(make_chunk(1, 0)));
    REQUIRE(q.enqueue(make_chunk(1, 1)));
    CHECK(q.enqueued() == 2);
    CHECK(q.dequeued() == 0);
    CHECK(q.drops() == 0);

    (void)q.dequeue_active(1);
    CHECK(q.dequeued() == 1);

    (void)q.invalidate_before(99);   // drops the remaining 1
    CHECK(q.drops() == 1);
    CHECK(q.size() == 0);
}

TEST_CASE("PlaybackQueue: concurrent producer/consumer don't lose chunks") {
    // Stress test the mutex-protected deque under simultaneous
    // enqueue/dequeue. Single producer, single consumer per the
    // declared threading contract.
    PlaybackQueue q(64);
    constexpr int N = 5000;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        for (SequenceNo i = 0; i < N; ++i) {
            // Backpressure: spin until accepted.
            while (!q.enqueue(make_chunk(7, i))) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        int last_seq = -1;
        while (consumed.load(std::memory_order_relaxed) < N) {
            auto c = q.dequeue_active(7);
            if (!c) {
                std::this_thread::yield();
                continue;
            }
            // Per-turn ordering preserved.
            const int s = static_cast<int>(c->seq);
            REQUIRE(s == last_seq + 1);
            last_seq = s;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();
    CHECK(q.enqueued() == static_cast<std::uint64_t>(N));
    CHECK(q.dequeued() == static_cast<std::uint64_t>(N));
    CHECK(q.size() == 0);
    // Every chunk landed eventually — `drops` counts how many times
    // the producer was rebuffed by capacity, which on a hot loop
    // without backpressure is non-zero. The contract is "no data
    // lost", not "no rejection pressure".
}
