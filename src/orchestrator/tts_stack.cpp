#include "orchestrator/tts_stack.hpp"

#include "audio/resampler.hpp"
#include "audio/utterance.hpp"
#include "dialogue/turn.hpp"
#include "log/log.hpp"
#include "stt/openai_stt_client.hpp"
#include "tts/types.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace acva::orchestrator {

namespace {

// Self-listen producer/consumer: one job per successfully synthesized
// sentence. Held in a shared_ptr deque so the bridge's hook (called
// on the bridge's I/O thread) and the worker thread can both touch
// it via the same shared mutex.
struct SelfListenJob {
    event::TurnId       turn;
    event::SequenceNo   seq;
    std::string         expected;
    std::string         lang;
    std::uint32_t       native_rate;
    std::vector<std::int16_t> samples;
};

// Background worker for the self-listen loop. Pops jobs, resamples
// to 16 kHz, POSTs to STT, logs the expected/heard pair as a
// structured `self_listen` event.
void self_listen_run(
    const config::SttConfig& stt_cfg,
    std::shared_ptr<std::deque<SelfListenJob>> jobs,
    std::shared_ptr<std::mutex> mu,
    std::shared_ptr<std::condition_variable> cv,
    std::shared_ptr<std::atomic<bool>> stop) {
    stt::OpenAiSttClient client(stt_cfg);
    while (!stop->load(std::memory_order_acquire)) {
        SelfListenJob job;
        {
            std::unique_lock lk(*mu);
            cv->wait(lk, [&]{
                return stop->load(std::memory_order_acquire) || !jobs->empty();
            });
            if (stop->load(std::memory_order_acquire) && jobs->empty()) return;
            job = std::move(jobs->front());
            jobs->pop_front();
        }
        // Resample native (e.g. 22050) → 16 kHz for STT.
        audio::Resampler r(static_cast<double>(job.native_rate), 16000.0,
                            audio::Resampler::Quality::High);
        auto resampled = r.process(job.samples);
        auto tail = r.flush();
        resampled.insert(resampled.end(), tail.begin(), tail.end());
        const auto now = std::chrono::steady_clock::now();
        auto slice = std::make_shared<audio::AudioSlice>(
            std::move(resampled), 16000U, now, now);

        std::string heard, err;
        stt::SttCallbacks cb;
        cb.on_final = [&](event::FinalTranscript ft) { heard = std::move(ft.text); };
        cb.on_error = [&](std::string e)             { err   = std::move(e); };
        const auto t0 = std::chrono::steady_clock::now();
        client.submit(stt::SttRequest{
            .turn = job.turn, .slice = slice,
            .cancel = std::make_shared<dialogue::CancellationToken>(),
            .lang_hint = job.lang,
        }, cb);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        // Single-source diff signal: char count + first ~600 chars
        // of each so the user can eyeball dropouts without grepping
        // the whole list. 600 covers the longest sentence the
        // splitter emits (cfg.dialogue.sentence_splitter.max_sentence_chars).
        auto trim = [](std::string s) {
            if (s.size() > 600) s = s.substr(0, 600) + "…";
            return s;
        };
        if (!err.empty()) {
            log::event("self_listen", "self_listen", job.turn,
                {{"seq", std::to_string(job.seq)},
                 {"expected_chars", std::to_string(job.expected.size())},
                 {"audio_ms", std::to_string(
                     job.samples.size() * 1000U / job.native_rate)},
                 {"stt_ms", std::to_string(ms)},
                 {"error", std::move(err)}});
        } else {
            log::event("self_listen", "self_listen", job.turn,
                {{"seq", std::to_string(job.seq)},
                 {"expected_chars", std::to_string(job.expected.size())},
                 {"heard_chars", std::to_string(heard.size())},
                 {"audio_ms", std::to_string(
                     job.samples.size() * 1000U / job.native_rate)},
                 {"stt_ms", std::to_string(ms)},
                 {"expected", trim(job.expected)},
                 {"heard", trim(heard)}});
        }
    }
}

} // namespace

TtsStack::~TtsStack() { stop(); }

void TtsStack::stop() {
    if (stopped_) return;
    stopped_ = true;

    // 1. No new TTS submissions.
    if (bridge_) bridge_->stop();

    // 2. Self-listen worker drains AFTER the bridge stops emitting
    // new jobs but BEFORE the playback engine winds down (so the
    // worker can finish its in-flight STT call without racing
    // teardown).
    if (self_listen_thread_.joinable()) {
        if (self_listen_stop_) {
            self_listen_stop_->store(true, std::memory_order_release);
        }
        if (self_listen_cv_) self_listen_cv_->notify_all();
        self_listen_thread_.join();
    }

    // 3. Engine. Drains the audio cb + publisher.
    if (engine_) engine_->stop();

    // 4. Metrics poller — last so it sees a clean final state.
    if (metrics_thread_.joinable()) {
        metrics_stop_.store(true, std::memory_order_release);
        metrics_thread_.join();
    }
}

std::unique_ptr<TtsStack>
build_tts_stack(const config::Config& cfg,
                 event::EventBus& bus,
                 const std::shared_ptr<metrics::Registry>& registry,
                 const std::shared_ptr<std::atomic<event::TurnId>>& playback_active_turn) {
    auto stack = std::make_unique<TtsStack>();

    if (cfg.tts.voices.empty()) {
        log::info("main", "tts disabled — cfg.tts.voices is empty");
        return stack;     // .enabled() == false
    }

    stack->queue_ = std::make_unique<playback::PlaybackQueue>(
        cfg.playback.max_queue_chunks);
    stack->engine_ = std::make_unique<playback::PlaybackEngine>(
        cfg.audio, cfg.playback, *stack->queue_, bus,
        [playback_active_turn]{
            return playback_active_turn->load(std::memory_order_acquire);
        });

    // M6 — wire the AEC reference tap. The PlaybackEngine writes
    // every emitted chunk into this ring (with a wall-clock
    // timestamp); the M4 capture pipeline pulls aligned reference
    // frames from it for APM. We construct it whenever capture is
    // ALSO enabled — there's no point taping playback if no one
    // will read it.
    if (cfg.audio.capture_enabled) {
        const std::size_t loopback_capacity =
            static_cast<std::size_t>(cfg.audio.loopback.ring_seconds)
            * static_cast<std::size_t>(cfg.audio.sample_rate_hz);
        stack->loopback_ = std::make_unique<audio::LoopbackSink>(
            loopback_capacity, cfg.audio.sample_rate_hz);
        stack->engine_->set_loopback_sink(stack->loopback_.get());
        log::info("main", fmt::format(
            "loopback ring: {}s × {}Hz = {} samples",
            cfg.audio.loopback.ring_seconds,
            cfg.audio.sample_rate_hz,
            loopback_capacity));
    }

    // M4B: TTS goes through Speaches via OpenAiTtsClient. The
    // bridge takes a generic submit callable so the client class
    // doesn't leak beyond this block.
    stack->client_ = std::make_unique<tts::OpenAiTtsClient>(cfg.tts);
    dialogue::TtsBridge::SubmitFn submit_fn =
        [c = stack->client_.get()](tts::TtsRequest r, tts::TtsCallbacks cb) {
            c->submit(std::move(r), std::move(cb));
        };
    log::info("main", fmt::format("tts (speaches): base_url={}", cfg.tts.base_url));

    stack->bridge_ = std::make_unique<dialogue::TtsBridge>(
        cfg, bus, std::move(submit_fn), *stack->queue_);

    stack->engine_->start();
    stack->bridge_->start();

    // M6 — self-listen feedback loop. Wired here so it has access
    // to the production STT client config; the bridge fires the
    // hook with native-rate samples after each successful
    // synthesis. Throttled to N concurrent in-flight STT calls so
    // it can't starve VRAM during normal operation.
    if (cfg.dialogue.self_listen.enabled && !cfg.stt.base_url.empty()) {
        auto sl_jobs = std::make_shared<std::deque<SelfListenJob>>();
        auto sl_mu   = std::make_shared<std::mutex>();
        auto sl_cv   = std::make_shared<std::condition_variable>();
        auto sl_stop = std::make_shared<std::atomic<bool>>(false);
        const auto sl_max = std::max<std::uint32_t>(
            1, cfg.dialogue.self_listen.max_in_flight);

        stack->bridge_->set_self_listen_sink(
            [sl_jobs, sl_mu, sl_cv, sl_max](
                event::TurnId turn, event::SequenceNo seq,
                std::string text, std::string lang,
                std::uint32_t rate, std::vector<std::int16_t> samples) {
                {
                    std::lock_guard lk(*sl_mu);
                    while (sl_jobs->size() >= sl_max * 4) sl_jobs->pop_front();
                    sl_jobs->push_back(SelfListenJob{
                        turn, seq, std::move(text), std::move(lang),
                        rate, std::move(samples)});
                }
                sl_cv->notify_one();
            });

        stack->self_listen_thread_ = std::thread(
            self_listen_run, std::cref(cfg.stt),
            sl_jobs, sl_mu, sl_cv, sl_stop);
        stack->self_listen_stop_ = sl_stop;
        stack->self_listen_cv_   = sl_cv;
        log::info("main",
            "self-listen enabled: every TTS sentence will be re-transcribed");
    }

    // Tiny poller thread: every 500 ms, push the engine + queue
    // counters into the metrics gauges. The audio thread can't touch
    // prometheus families directly (locks) so this side-thread owns
    // the bridging.
    stack->metrics_thread_ = std::thread([stack_ptr = stack.get(), registry]{
        using namespace std::chrono_literals;
        while (!stack_ptr->metrics_stop_.load(std::memory_order_acquire)) {
            registry->set_playback_queue_depth(
                static_cast<double>(stack_ptr->queue_->size()));
            registry->set_playback_underruns_total(
                static_cast<double>(stack_ptr->engine_->underruns()));
            registry->set_playback_chunks_played_total(
                static_cast<double>(stack_ptr->engine_->chunks_played()));
            registry->set_playback_drops_total(
                static_cast<double>(stack_ptr->queue_->drops()));
            // M7 — drain any pending barge-in latency the audio
            // thread stashed since the last poll. Each successful
            // pop observes once into the histogram; subsequent
            // polls return -1 until the next barge-in.
            const double bi_ms =
                stack_ptr->engine_->consume_pending_barge_in_latency_ms();
            if (bi_ms >= 0.0) {
                registry->on_barge_in_latency(bi_ms);
            }
            std::this_thread::sleep_for(500ms);
        }
    });

    log::info("main", fmt::format(
        "tts enabled — voices configured: {} (audio: {} headless={})",
        cfg.tts.voices.size(),
        cfg.audio.output_device,
        stack->engine_->headless()));
    return stack;
}

} // namespace acva::orchestrator
