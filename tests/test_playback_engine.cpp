#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using acva::config::AudioConfig;
using acva::dialogue::TurnId;
using acva::event::EventBus;
using acva::event::PlaybackFinished;
using acva::event::SequenceNo;
using acva::playback::AudioChunk;
using acva::playback::PlaybackEngine;
using acva::playback::PlaybackQueue;

namespace {

AudioConfig headless_cfg(std::size_t buffer_frames = 64) {
    AudioConfig c;
    c.output_device = "none";   // skip PortAudio init entirely
    c.sample_rate_hz = 48000;
    c.buffer_frames = static_cast<std::uint32_t>(buffer_frames);
    return c;
}

AudioChunk chunk(TurnId turn, SequenceNo seq, std::size_t n_samples,
                  std::int16_t fill = 1) {
    AudioChunk c;
    c.turn = turn;
    c.seq  = seq;
    c.samples.assign(n_samples, fill);
    return c;
}

// Captures PlaybackFinished events; the publisher thread inside
// PlaybackEngine relays from its postbox onto the bus.
class FinishedSink {
public:
    explicit FinishedSink(EventBus& bus) {
        sub_ = bus.subscribe<PlaybackFinished>({},
            [this](const PlaybackFinished& e) {
                std::lock_guard lk(mu_);
                events_.push_back(e);
            });
    }
    ~FinishedSink() { if (sub_) sub_->stop(); }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(mu_); return events_.size();
    }
    [[nodiscard]] std::vector<PlaybackFinished> snapshot() const {
        std::lock_guard lk(mu_); return events_;
    }
private:
    mutable std::mutex mu_;
    std::vector<PlaybackFinished> events_;
    acva::event::SubscriptionHandle sub_;
};

template <class Pred>
bool wait_for(Pred p,
               std::chrono::milliseconds budget = std::chrono::milliseconds(800)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

} // namespace

TEST_CASE("PlaybackEngine: headless mode drains queue and emits PlaybackFinished") {
    AudioConfig cfg = headless_cfg(32);
    PlaybackQueue q(8);
    EventBus bus;
    FinishedSink sink(bus);

    std::atomic<TurnId> active{1};
    PlaybackEngine eng(cfg, q, bus, [&]{ return active.load(); });
    eng.force_headless(std::chrono::milliseconds(2));
    REQUIRE(eng.start());
    CHECK(eng.headless());
    CHECK(eng.running());

    REQUIRE(q.enqueue(chunk(1, 0, 100)));
    REQUIRE(q.enqueue(chunk(1, 1, 100)));
    REQUIRE(q.enqueue(chunk(1, 2, 100)));

    // Wait for both chunks to be consumed and PlaybackFinished events
    // to come through the publisher thread.
    REQUIRE(wait_for([&]{ return sink.size() >= 3; }));
    eng.stop();

    auto evs = sink.snapshot();
    REQUIRE(evs.size() >= 3);
    CHECK(evs[0].seq == 0);
    CHECK(evs[1].seq == 1);
    CHECK(evs[2].seq == 2);
    CHECK(eng.chunks_played() >= 3);
    CHECK(eng.frames_played() > 0);
}

TEST_CASE("PlaybackEngine: empty queue drives underruns to non-zero, no crash") {
    AudioConfig cfg = headless_cfg(16);
    PlaybackQueue q(8);
    EventBus bus;
    FinishedSink sink(bus);

    std::atomic<TurnId> active{1};
    PlaybackEngine eng(cfg, q, bus, [&]{ return active.load(); });
    eng.force_headless(std::chrono::milliseconds(2));
    REQUIRE(eng.start());

    REQUIRE(wait_for([&]{ return eng.underruns() > 0; }));
    eng.stop();

    CHECK(eng.underruns() > 0);
    CHECK(sink.size() == 0);   // nothing was queued
}

TEST_CASE("PlaybackEngine: stale-turn chunks are dropped, never played") {
    AudioConfig cfg = headless_cfg(32);
    PlaybackQueue q(16);
    EventBus bus;
    FinishedSink sink(bus);

    std::atomic<TurnId> active{42};
    PlaybackEngine eng(cfg, q, bus, [&]{ return active.load(); });
    eng.force_headless(std::chrono::milliseconds(2));
    REQUIRE(eng.start());

    // Queue with a mix of stale and active turns. Only the active
    // ones should produce PlaybackFinished events.
    REQUIRE(q.enqueue(chunk(40, 0, 50)));   // stale (smaller turn id)
    REQUIRE(q.enqueue(chunk(41, 0, 50)));   // stale
    REQUIRE(q.enqueue(chunk(42, 0, 50)));   // active
    REQUIRE(q.enqueue(chunk(42, 1, 50)));   // active

    REQUIRE(wait_for([&]{ return sink.size() >= 2; }));
    eng.stop();

    auto evs = sink.snapshot();
    for (const auto& e : evs) CHECK(e.turn == 42);
    CHECK(eng.chunks_played() >= 2);
    CHECK(q.drops() >= 2);   // the two stale chunks were dropped
}

TEST_CASE("PlaybackEngine: render_into is callable directly (no thread needed)") {
    // Direct test of the audio-thread entry point without spinning up
    // PortAudio or the headless ticker. Useful as a unit test for the
    // chunk-to-buffer state machine.
    AudioConfig cfg = headless_cfg(8);
    PlaybackQueue q(4);
    EventBus bus;
    FinishedSink sink(bus);

    std::atomic<TurnId> active{1};
    PlaybackEngine eng(cfg, q, bus, [&]{ return active.load(); });
    // Manual mode — start the publisher but don't kick off either
    // PortAudio or the headless ticker. We need start() because that
    // spins up the publisher thread that reads the postbox.
    eng.force_headless(std::chrono::seconds(60));   // effectively never tick
    REQUIRE(eng.start());

    REQUIRE(q.enqueue(chunk(1, 0, 4, 100)));
    REQUIRE(q.enqueue(chunk(1, 1, 4, 200)));

    std::vector<std::int16_t> out(8);
    eng.render_into(out.data(), out.size());

    // First 4 samples come from chunk 0, next 4 from chunk 1.
    for (std::size_t i = 0; i < 4; ++i) CHECK(out[i] == 100);
    for (std::size_t i = 4; i < 8; ++i) CHECK(out[i] == 200);

    // PlaybackFinished for chunk 0 was posted when chunk 1 started
    // streaming. Chunk 1 hasn't completed yet (its samples are still
    // mid-flight in the cursor) so only one event is on the bus.
    REQUIRE(wait_for([&]{ return sink.size() >= 1; }));

    // Drive one more buffer with the queue empty to flush the cursor.
    std::vector<std::int16_t> tail(8);
    eng.render_into(tail.data(), tail.size());

    REQUIRE(wait_for([&]{ return sink.size() >= 2; }));
    auto evs = sink.snapshot();
    CHECK(evs[0].seq == 0);
    CHECK(evs[1].seq == 1);
    CHECK(eng.underruns() >= 1);   // second render hit empty queue

    eng.stop();
}

TEST_CASE("PlaybackEngine: stop is idempotent") {
    AudioConfig cfg = headless_cfg();
    PlaybackQueue q(4);
    EventBus bus;
    PlaybackEngine eng(cfg, q, bus, []{ return TurnId{1}; });
    eng.force_headless(std::chrono::milliseconds(2));
    REQUIRE(eng.start());
    eng.stop();
    eng.stop();    // no-op
    CHECK_FALSE(eng.running());
}
