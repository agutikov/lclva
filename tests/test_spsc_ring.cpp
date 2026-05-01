#include "audio/spsc_ring.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using acva::audio::SpscRing;

TEST_CASE("SpscRing: push/pop round-trips a single value") {
    SpscRing<int, 16> ring;
    CHECK(ring.empty());
    CHECK(ring.push(42));
    CHECK(!ring.empty());
    auto v = ring.pop();
    REQUIRE(v.has_value());
    CHECK(*v == 42);
    CHECK(ring.empty());
}

TEST_CASE("SpscRing: pop on empty returns nullopt") {
    SpscRing<int, 16> ring;
    CHECK(!ring.pop().has_value());
}

TEST_CASE("SpscRing: rejects push when full") {
    SpscRing<int, 4> ring;            // usable capacity 3
    CHECK(ring.push(1));
    CHECK(ring.push(2));
    CHECK(ring.push(3));
    CHECK(!ring.push(4));              // full
    CHECK(ring.size() == 3);
}

TEST_CASE("SpscRing: FIFO order across many push/pop cycles") {
    SpscRing<int, 8> ring;
    for (int batch = 0; batch < 4; ++batch) {
        for (int i = 0; i < 6; ++i) {
            CHECK(ring.push(batch * 100 + i));
        }
        for (int i = 0; i < 6; ++i) {
            auto v = ring.pop();
            REQUIRE(v.has_value());
            CHECK(*v == batch * 100 + i);
        }
    }
}

TEST_CASE("SpscRing: SPSC stress — every produced item is consumed in order") {
    constexpr std::size_t kCapacity = 1024;
    constexpr std::uint64_t kCount  = 100'000;
    SpscRing<std::uint64_t, kCapacity> ring;

    std::atomic<bool> producer_done{false};
    std::vector<std::uint64_t> consumed;
    consumed.reserve(kCount);

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kCount) {
            auto v = ring.pop();
            if (!v) {
                if (producer_done.load(std::memory_order_acquire) && ring.empty()) {
                    // Producer might have produced all-but-our-pop's slot.
                    auto last = ring.pop();
                    if (!last) break;
                    consumed.push_back(*last);
                    REQUIRE(*last == expected);
                    ++expected;
                }
                std::this_thread::yield();
                continue;
            }
            REQUIRE(*v == expected);
            consumed.push_back(*v);
            ++expected;
        }
    });

    for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!ring.push(i)) {
            std::this_thread::yield();
        }
    }
    producer_done.store(true, std::memory_order_release);

    consumer.join();
    CHECK(consumed.size() == kCount);
    for (std::size_t i = 0; i < consumed.size(); ++i) {
        CHECK(consumed[i] == static_cast<std::uint64_t>(i));
    }
}

TEST_CASE("SpscRing: capacity reports usable capacity") {
    SpscRing<int, 8> ring;
    CHECK(ring.capacity() == 7);
    for (int i = 0; i < 7; ++i) CHECK(ring.push(i));
    CHECK(!ring.push(99));
}
