#include "orchestrator/stt_stack.hpp"

#include "dialogue/turn.hpp"
#include "event/event.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <chrono>
#include <span>
#include <string>
#include <utility>

namespace acva::orchestrator {

SttStack::~SttStack() { stop(); }

void SttStack::stop() {
    if (stopped_) return;
    stopped_ = true;

    if (realtime_) realtime_->stop();
    if (worker_.joinable()) {
        stop_.store(true, std::memory_order_release);
        queue_cv_.notify_all();
        worker_.join();
    }
}

std::unique_ptr<SttStack>
build_stt_stack(const config::Config& cfg,
                 event::EventBus& bus,
                 audio::AudioPipeline* audio_pipeline,
                 std::vector<event::SubscriptionHandle>& subscription_keepalive) {
    auto stack = std::make_unique<SttStack>();

    if (cfg.stt.base_url.empty()) {
        log::info("main", "stt disabled (cfg.stt.base_url empty)");
        return stack;     // .enabled() == false
    }

    if (cfg.stt.streaming && cfg.audio.capture_enabled) {
        // M5 streaming path. Realtime client owns the WebRTC session.
        stack->realtime_ = std::make_unique<stt::RealtimeSttClient>(cfg.stt);
        const bool ok = stack->realtime_->start(std::chrono::seconds(15));
        if (!ok) {
            log::warn("main",
                "realtime stt failed to start; dialogue path will not "
                "receive transcripts (check `acva demo health` for Speaches)");
            stack->realtime_.reset();
            return stack;
        }

        // Wire the live audio sink — pipeline pushes chunks, client
        // bridges to Speaches over the data channel. Only when
        // capture is enabled; otherwise the sink stays disconnected.
        if (audio_pipeline) {
            auto* rt = stack->realtime_.get();
            audio_pipeline->set_live_audio_sink(
                [rt](std::span<const std::int16_t> samples) {
                    rt->push_audio(samples);
                });
        }

        // begin_utterance on SpeechStarted, end_utterance on
        // SpeechEnded. Callbacks publish PartialTranscript /
        // FinalTranscript on the bus so the dialogue Manager
        // consumes them transparently. turn=NoTurn — the FSM
        // doesn't validate turn ids on these events.
        auto* rt = stack->realtime_.get();
        subscription_keepalive.push_back(bus.subscribe<event::SpeechStarted>({},
            [rt, &bus](const event::SpeechStarted&) {
                auto cancel = std::make_shared<dialogue::CancellationToken>();
                stt::RealtimeSttClient::UtteranceCallbacks cb;
                cb.on_partial = [&bus](event::PartialTranscript p) {
                    bus.publish(std::move(p));
                };
                cb.on_final = [&bus](event::FinalTranscript f) {
                    bus.publish(std::move(f));
                };
                cb.on_error = [](std::string err) {
                    log::warn("stt",
                        fmt::format("realtime transcription failed: {}", err));
                };
                rt->begin_utterance(event::kNoTurn, cancel, std::move(cb));
            }));
        subscription_keepalive.push_back(bus.subscribe<event::SpeechEnded>({},
            [rt](const event::SpeechEnded&) {
                rt->end_utterance();
            }));

        log::info("main", fmt::format(
            "stt enabled (streaming, base_url={}, model={})",
            cfg.stt.base_url, cfg.stt.model));
    } else {
        // M4B request/response path. One worker thread drains a
        // queue of SttRequests fed by UtteranceReady events.
        stack->request_response_ = std::make_unique<stt::OpenAiSttClient>(cfg.stt);

        subscription_keepalive.push_back(bus.subscribe<event::UtteranceReady>({},
            [stack_ptr = stack.get(), lang = cfg.stt.language](
                const event::UtteranceReady& e) {
                if (!e.slice) return;
                std::lock_guard lk(stack_ptr->queue_mu_);
                stack_ptr->queue_.push_back(stt::SttRequest{
                    .turn  = e.turn,
                    .slice = e.slice,
                    .cancel = std::make_shared<dialogue::CancellationToken>(),
                    .lang_hint = lang,
                });
                stack_ptr->queue_cv_.notify_one();
            }));

        stack->worker_ = std::thread([stack_ptr = stack.get(), &bus]{
            while (!stack_ptr->stop_.load(std::memory_order_acquire)) {
                stt::SttRequest req;
                {
                    std::unique_lock lk(stack_ptr->queue_mu_);
                    stack_ptr->queue_cv_.wait(lk, [&]{
                        return stack_ptr->stop_.load(std::memory_order_acquire)
                            || !stack_ptr->queue_.empty();
                    });
                    if (stack_ptr->stop_.load(std::memory_order_acquire)) return;
                    req = std::move(stack_ptr->queue_.front());
                    stack_ptr->queue_.pop_front();
                }
                stt::SttCallbacks cb;
                cb.on_final = [&bus](event::FinalTranscript ft) {
                    bus.publish(std::move(ft));
                };
                cb.on_error = [](std::string err) {
                    log::warn("stt",
                        fmt::format("transcription failed: {}", err));
                };
                stack_ptr->request_response_->submit(std::move(req), std::move(cb));
            }
        });

        log::info("main", fmt::format(
            "stt enabled (request/response, base_url={}, model={})",
            cfg.stt.base_url, cfg.stt.model));
    }
    return stack;
}

} // namespace acva::orchestrator
