#include "config/config.hpp"
#include "dialogue/tts_bridge.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/queue.hpp"
#include "tts/openai_tts_client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using acva::config::Config;
using acva::config::TtsVoice;
using acva::dialogue::TtsBridge;
using acva::event::EventBus;
using acva::event::TtsAudioChunk;
using acva::event::TtsFinished;
using acva::event::TtsStarted;
using acva::playback::PlaybackQueue;
using acva::tts::OpenAiTtsClient;

namespace {

// In-process Speaches stub: answers `POST /v1/audio/speech` with bare
// int16 mono 22050 Hz PCM (the same `response_format=pcm` shape the
// production OpenAiTtsClient consumes). `/health` answers 200 so the
// probe path is exercised too.
class FakeSpeaches {
public:
    FakeSpeaches(std::uint32_t rate_hz = 22050, std::size_t n_samples = 256,
                  std::chrono::milliseconds delay = {})
        : rate_(rate_hz), n_(n_samples), delay_(delay) {
        srv_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });
        srv_.Post("/v1/audio/speech",
            [this](const httplib::Request&, httplib::Response& res) {
                if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
                std::vector<std::int16_t> s(n_, static_cast<std::int16_t>(7000));
                res.status = 200;
                res.set_content(
                    std::string(reinterpret_cast<const char*>(s.data()),
                                s.size() * sizeof(std::int16_t)),
                    "application/octet-stream");
            });
        port_ = srv_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
        for (int i = 0; i < 200 && !srv_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ~FakeSpeaches() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] std::uint32_t rate() const { return rate_; }

private:
    httplib::Server srv_;
    std::thread thread_;
    int port_ = 0;
    std::uint32_t rate_;
    std::size_t   n_;
    std::chrono::milliseconds delay_{};
};

Config make_cfg(const std::string& base_url) {
    Config c;
    c.tts.base_url = base_url;
    c.tts.voices["en"] = TtsVoice{
        .model_id = "speaches-ai/piper-en_US-amy-medium",
        .voice_id = "amy",
    };
    c.tts.fallback_lang = "en";
    c.tts.request_timeout_seconds = 5;
    c.audio.sample_rate_hz = 48000;
    c.audio.buffer_frames = 480;
    c.dialogue.max_tts_queue_sentences = 16;
    return c;
}

template <class T>
class Sink {
public:
    explicit Sink(EventBus& bus) {
        sub_ = bus.subscribe<T>({}, [this](const T& e) {
            std::lock_guard lk(mu_); events_.push_back(e);
        });
    }
    ~Sink() { if (sub_) sub_->stop(); }
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(mu_); return events_.size();
    }
    [[nodiscard]] std::vector<T> snapshot() const {
        std::lock_guard lk(mu_); return events_;
    }
private:
    mutable std::mutex mu_;
    std::vector<T> events_;
    acva::event::SubscriptionHandle sub_;
};

template <class Pred>
bool wait_for(Pred p, std::chrono::milliseconds budget = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

// Drain a PlaybackQueue: returns the total sample count across all
// chunks for `active_turn`. Stops once empty.
std::size_t drain_samples(PlaybackQueue& q, acva::dialogue::TurnId turn) {
    std::size_t n = 0;
    while (auto c = q.dequeue_active(turn)) n += c->samples.size();
    return n;
}

} // namespace

TEST_CASE("TtsBridge: LlmSentence drives Piper synth → resampled audio in queue + events") {
    FakeSpeaches piper(22050, 220);   // ~10ms of audio at 22050 Hz
    auto cfg = make_cfg(piper.base_url());
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(64);
    Sink<TtsStarted>    started(bus);
    Sink<TtsAudioChunk> chunks(bus);
    Sink<TtsFinished>   finished(bus);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();

    bus.publish(acva::event::LlmStarted{ .turn = 7 });
    bus.publish(acva::event::LlmSentence{
        .turn = 7, .seq = 0, .text = "hello world", .lang = "en",
    });

    // Each sink runs on its own bus-subscriber worker thread; wait for
    // each independently to avoid a dispatch race.
    REQUIRE(wait_for([&]{ return started.size()  >= 1; }));
    REQUIRE(wait_for([&]{ return chunks.size()   >= 1; }));
    REQUIRE(wait_for([&]{ return finished.size() >= 1; }));
    bridge.stop();

    CHECK(started.size()  == 1);
    CHECK(finished.size() == 1);
    CHECK(chunks.size()   >= 1);

    // 220 samples @ 22050 → ≈ 479 samples @ 48000. Allow ±5%.
    const auto played = drain_samples(queue, 7);
    CHECK(played > 220 * 2);   // upsampled ratio is roughly 48/22 ≈ 2.18
    CHECK(played < 220 * 3);
    CHECK(bridge.sentences_synthesized() == 1);
    CHECK(bridge.sentences_cancelled()   == 0);
    CHECK(bridge.sentences_errored()     == 0);
}

TEST_CASE("TtsBridge: multiple sentences serialize through one I/O thread") {
    FakeSpeaches piper(22050, 100);
    auto cfg = make_cfg(piper.base_url());
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(64);
    Sink<TtsFinished> finished(bus);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();

    bus.publish(acva::event::LlmStarted{ .turn = 1 });
    for (acva::event::SequenceNo i = 0; i < 4; ++i) {
        bus.publish(acva::event::LlmSentence{
            .turn = 1, .seq = i, .text = "s", .lang = "en",
        });
    }

    REQUIRE(wait_for([&]{ return finished.size() >= 4; }));
    bridge.stop();

    auto fs = finished.snapshot();
    REQUIRE(fs.size() == 4);
    // I/O thread is serial: the events must arrive in seq order.
    for (acva::event::SequenceNo i = 0; i < 4; ++i) CHECK(fs[i].seq == i);
    CHECK(bridge.sentences_synthesized() == 4);
}

TEST_CASE("TtsBridge: UserInterrupted cancels in-flight, drains queue, drops pending") {
    // Slow server so the bridge has a sentence in flight when the
    // interrupt arrives.
    FakeSpeaches piper(22050, 100, std::chrono::milliseconds(150));
    auto cfg = make_cfg(piper.base_url());
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(64);
    Sink<TtsFinished> finished(bus);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();

    bus.publish(acva::event::LlmStarted{ .turn = 9 });
    // Three sentences: first in flight, two pending.
    for (acva::event::SequenceNo i = 0; i < 3; ++i) {
        bus.publish(acva::event::LlmSentence{
            .turn = 9, .seq = i, .text = "s", .lang = "en",
        });
    }

    // Wait for the bridge to start the first job (pending shrinks to 2).
    REQUIRE(wait_for([&]{ return bridge.pending() <= 2; }));

    bus.publish(acva::event::UserInterrupted{
        .turn = 9, .ts = std::chrono::steady_clock::now(),
    });

    REQUIRE(wait_for([&]{ return bridge.sentences_cancelled() >= 1; }));
    bridge.stop();

    // No TtsFinished should have been published — every sentence was
    // cancelled either in flight or before submission.
    CHECK(finished.size() == 0);
    CHECK(bridge.sentences_cancelled() >= 2);

    // The playback queue was drained on barge-in. The audio engine
    // would see nothing here.
    CHECK(queue.size() == 0);
}

TEST_CASE("TtsBridge: drops sentences arriving after their turn is cancelled") {
    FakeSpeaches piper(22050, 100);
    auto cfg = make_cfg(piper.base_url());
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(64);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();

    bus.publish(acva::event::LlmStarted{ .turn = 5 });
    // Cancel before any sentence arrives.
    bus.publish(acva::event::UserInterrupted{
        .turn = 5, .ts = std::chrono::steady_clock::now(),
    });
    // Now publish a sentence for that turn — bridge should refuse to
    // synthesize it.
    bus.publish(acva::event::LlmSentence{
        .turn = 5, .seq = 0, .text = "should not be spoken", .lang = "en",
    });

    // Give the subscriber thread time to dispatch.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    bridge.stop();

    CHECK(bridge.sentences_synthesized() == 0);
    CHECK(queue.size() == 0);
}

TEST_CASE("TtsBridge: lang miss falls back to fallback voice") {
    FakeSpeaches piper(22050, 100);
    auto cfg = make_cfg(piper.base_url());        // fallback = "en"
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(32);
    Sink<TtsFinished> finished(bus);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();

    bus.publish(acva::event::LlmStarted{ .turn = 1 });
    bus.publish(acva::event::LlmSentence{
        .turn = 1, .seq = 0, .text = "x", .lang = "zz",   // missing
    });

    REQUIRE(wait_for([&]{ return finished.size() >= 1; }));
    bridge.stop();
    CHECK(bridge.sentences_errored() == 0);
}

TEST_CASE("TtsBridge: stop while in-flight aborts cleanly") {
    FakeSpeaches piper(22050, 100, std::chrono::milliseconds(200));
    auto cfg = make_cfg(piper.base_url());
    EventBus bus;
    OpenAiTtsClient client(cfg.tts);
    PlaybackQueue queue(32);

    TtsBridge bridge(cfg, bus,
        [&](acva::tts::TtsRequest r, acva::tts::TtsCallbacks cb) {
            client.submit(std::move(r), std::move(cb));
        }, queue);
    bridge.start();
    bus.publish(acva::event::LlmStarted{ .turn = 1 });
    bus.publish(acva::event::LlmSentence{
        .turn = 1, .seq = 0, .text = "long", .lang = "en",
    });
    // Don't wait — stop while it's still running.
    REQUIRE(wait_for([&]{ return bridge.pending() == 0; }));
    const auto t0 = std::chrono::steady_clock::now();
    bridge.stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // Stop must complete much faster than the server's 200 ms delay
    // — that's the cancellation-on-shutdown contract.
    CHECK(elapsed < std::chrono::milliseconds(150));
}
