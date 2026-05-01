#include "demos/demo.hpp"

#include "dialogue/tts_bridge.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "playback/engine.hpp"
#include "playback/queue.hpp"
#include "tts/openai_tts_client.hpp"
#include "tts/piper_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>

namespace acva::demos {

int run_tts(const config::Config& cfg) {
    using namespace std::chrono_literals;

    if (cfg.tts.voices.empty()) {
        std::fprintf(stderr,
            "demo[tts] FAIL: cfg.tts.voices is empty — populate it before running\n");
        return EXIT_FAILURE;
    }
    const auto& fb = cfg.tts.fallback_lang;
    // Describe the route to stdout. Speaches uses one base_url +
    // model_id; Piper uses per-language url.
    std::string route_desc;
    if (cfg.tts.provider == "speaches") {
        route_desc = "speaches " + cfg.tts.base_url + " model_id=";
        if (cfg.tts.voices.contains(fb)) route_desc += cfg.tts.voices.at(fb).model_id;
    } else {
        route_desc = "piper url=";
        if (cfg.tts.voices.contains(fb)) route_desc += cfg.tts.voices.at(fb).url;
    }

    constexpr event::TurnId   kTurn = 1;
    constexpr event::SequenceNo kSeq  = 0;
    constexpr std::string_view kText = "Hello from acva.";

    std::printf(
        "demo[tts] synthesizing \"%.*s\" via lang='%s' [%s]\n",
        static_cast<int>(kText.size()), kText.data(),
        fb.c_str(), route_desc.c_str());

    event::EventBus bus;
    playback::PlaybackQueue queue(cfg.playback.max_queue_chunks);

    // Pick the client to mirror main.cpp's runtime path.
    std::unique_ptr<tts::PiperClient>      piper;
    std::unique_ptr<tts::OpenAiTtsClient>  openai;
    dialogue::TtsBridge::SubmitFn submit_fn;
    if (cfg.tts.provider == "speaches") {
        openai = std::make_unique<tts::OpenAiTtsClient>(cfg.tts);
        submit_fn = [c = openai.get()](tts::TtsRequest r, tts::TtsCallbacks cb) {
            c->submit(std::move(r), std::move(cb));
        };
    } else {
        piper = std::make_unique<tts::PiperClient>(cfg.tts);
        submit_fn = [c = piper.get()](tts::TtsRequest r, tts::TtsCallbacks cb) {
            c->submit(std::move(r), std::move(cb));
        };
    }
    playback::PlaybackEngine engine(cfg.audio, queue, bus,
                                      []{ return kTurn; });
    dialogue::TtsBridge     bridge(cfg, bus, std::move(submit_fn), queue);

    // Capture the first TtsAudioChunk for latency reporting; capture
    // any error the bridge surfaces.
    std::mutex mu;
    bool       got_first_audio = false;
    std::chrono::steady_clock::time_point first_audio_at;
    std::atomic<bool> finished{false};
    std::atomic<int>  errors{0};

    auto sub_audio = bus.subscribe<event::TtsAudioChunk>({},
        [&](const event::TtsAudioChunk&) {
            std::lock_guard lk(mu);
            if (!got_first_audio) {
                got_first_audio = true;
                first_audio_at = std::chrono::steady_clock::now();
            }
        });
    auto sub_finished = bus.subscribe<event::TtsFinished>({},
        [&](const event::TtsFinished&) { finished.store(true); });
    auto sub_error = bus.subscribe<event::ErrorEvent>({},
        [&](const event::ErrorEvent& e) {
            if (e.component == "tts_bridge" || e.component == "piper"
                || e.component == "openai_tts") {
                errors.fetch_add(1);
                std::fprintf(stderr,
                    "demo[tts] bridge error: %s\n", e.message.c_str());
            }
        });

    if (!engine.start()) {
        std::fprintf(stderr, "demo[tts] engine.start() failed\n");
        return EXIT_FAILURE;
    }
    bridge.start();
    std::printf("demo[tts] engine running (headless=%s)\n",
                 engine.headless() ? "true" : "false");

    // Drive the bridge as if a real LLM produced one sentence.
    const auto t0 = std::chrono::steady_clock::now();
    bus.publish(event::LlmStarted{ .turn = kTurn });
    bus.publish(event::LlmSentence{
        .turn = kTurn, .seq = kSeq,
        .text = std::string{kText},
        .lang = fb,
    });

    // Wait for TtsFinished or the synthesis-timeout from config.
    const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(
                                  cfg.tts.request_timeout_seconds + 5);
    while (!finished.load(std::memory_order_acquire)
           && errors.load(std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    bus.publish(event::LlmFinished{
        .turn = kTurn, .cancelled = false, .tokens_generated = 0,
    });

    // Drain playback after Piper is done. The active turn stays at
    // kTurn so the engine can fully consume the queue.
    const auto play_deadline = std::chrono::steady_clock::now() + 8s;
    while (queue.size() > 0 || engine.chunks_played() == 0) {
        if (std::chrono::steady_clock::now() >= play_deadline) break;
        std::this_thread::sleep_for(20ms);
    }
    std::this_thread::sleep_for(150ms);   // settle the trailing chunk

    bridge.stop();
    engine.stop();
    sub_audio->stop();
    sub_finished->stop();
    sub_error->stop();
    bus.shutdown();

    if (errors.load() > 0) {
        std::fprintf(stderr,
            "demo[tts] FAIL: %d error event(s) on the bus — see logs above. "
            "Is the configured TTS backend reachable? (%s)\n",
            errors.load(), route_desc.c_str());
        return EXIT_FAILURE;
    }
    if (!finished.load()) {
        std::fprintf(stderr,
            "demo[tts] FAIL: TtsFinished never fired (timeout). "
            "Is `docker compose up -d` running and the TTS backend alive? "
            "(%s)\n", route_desc.c_str());
        return EXIT_FAILURE;
    }

    double first_audio_ms = -1.0;
    {
        std::lock_guard lk(mu);
        if (got_first_audio) {
            first_audio_ms = std::chrono::duration<double, std::milli>(
                                  first_audio_at - t0).count();
        }
    }
    std::printf(
        "demo[tts] done: first_audio_ms=%.1f chunks_played=%llu "
        "underruns=%llu synthesized=%llu\n",
        first_audio_ms,
        static_cast<unsigned long long>(engine.chunks_played()),
        static_cast<unsigned long long>(engine.underruns()),
        static_cast<unsigned long long>(bridge.sentences_synthesized()));

    if (engine.headless()) {
        std::printf(
            "demo[tts] NOTE: engine ran headless — Piper synth was verified, "
            "but no audio reached speakers. Set audio.output_device to a real device.\n");
    }
    return EXIT_SUCCESS;
}

} // namespace acva::demos
