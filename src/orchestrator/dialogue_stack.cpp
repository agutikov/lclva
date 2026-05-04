#include "orchestrator/dialogue_stack.hpp"

#include "log/log.hpp"
#include "memory/repository.hpp"

#include <fmt/format.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>

namespace acva::orchestrator {

DialogueStack::~DialogueStack() { stop(); }

void DialogueStack::stop() {
    if (stopped_) return;
    stopped_ = true;

    if (fake_driver_) fake_driver_->stop();
    // Stop barge-in detection before tearing down the LLM/manager so a
    // late SpeechStarted can't fire UserInterrupted into a half-shut
    // pipeline.
    if (barge_in_)    barge_in_->stop();
    if (keep_alive_)  keep_alive_->stop();   // before llm_client teardown
    if (manager_)     manager_->stop();
    if (turn_writer_) turn_writer_->stop();
    if (summarizer_)  summarizer_->stop();
}

std::variant<std::unique_ptr<DialogueStack>, memory::DbError>
build_dialogue_stack(const config::Config& cfg,
                      event::EventBus& bus,
                      const std::shared_ptr<metrics::Registry>& registry,
                      memory::MemoryThread& memory,
                      dialogue::Fsm& fsm,
                      supervisor::Supervisor& sup,
                      dialogue::TurnFactory& turns,
                      const audio::Apm* apm,
                      const std::shared_ptr<std::atomic<event::TurnId>>& playback_active_turn,
                      std::vector<event::SubscriptionHandle>& subscription_keepalive) {
    auto stack = std::make_unique<DialogueStack>();

    // Optional fake pipeline driver (M0). Mutually exclusive with
    // real STT, but coexists with the LLM stack.
    if (cfg.pipeline.fake_driver_enabled) {
        pipeline::FakeDriverOptions opts;
        opts.sentences_per_turn   = cfg.pipeline.fake_sentences_per_turn;
        opts.idle_between_turns   = std::chrono::milliseconds{
            cfg.pipeline.fake_idle_between_turns_ms};
        opts.barge_in_probability = cfg.pipeline.fake_barge_in_probability;
        opts.suppress_speech_events = cfg.audio.capture_enabled;
        stack->fake_driver_ = std::make_unique<pipeline::FakeDriver>(bus, opts);
        stack->fake_driver_->start();
        log::info("main", "fake pipeline driver enabled");
    } else {
        log::info("main", "fake pipeline driver disabled");
    }

    if (cfg.llm.base_url.empty()) {
        log::info("main", "llm disabled (cfg.llm.base_url empty); "
                          "FinalTranscript events will have no consumer");
        return stack;     // .has_llm() == false
    }

    // Open a fresh memory session before constructing the LLM-driven
    // components. Without this, every dialogue turn fails to write
    // back into memory.
    auto sid_or = memory.read([](memory::Repository& repo) {
        return repo.insert_session(memory::now_ms(), std::nullopt);
    });
    if (auto* err = std::get_if<memory::DbError>(&sid_or)) {
        return *err;
    }
    const auto session_id = std::get<memory::SessionId>(sid_or);
    log::event("main", "session_opened", event::kNoTurn,
               {{"session_id", std::to_string(session_id)}});

    stack->prompt_builder_ = std::make_unique<llm::PromptBuilder>(cfg, memory);
    stack->llm_client_     = std::make_unique<llm::LlmClient>(cfg, bus);
    stack->manager_        = std::make_unique<dialogue::Manager>(
        cfg, bus, *stack->prompt_builder_, *stack->llm_client_, turns);
    stack->turn_writer_    = std::make_unique<dialogue::TurnWriter>(bus, memory);
    stack->summarizer_     = std::make_unique<memory::Summarizer>(
        cfg, bus, memory, *stack->llm_client_);
    stack->manager_->set_session(session_id);
    stack->turn_writer_->set_session(session_id);
    stack->summarizer_->set_session(session_id);

    // M2: pipeline gating + LLM keep-alive. The gate refuses new
    // turns when the supervisor reports pipeline_state==Failed; the
    // keep-alive timer pings llama every keep_alive_interval_seconds
    // while the FSM is Listening so the model stays loaded.
    stack->manager_->set_pipeline_gate([&sup]{
        return sup.pipeline_state() != supervisor::PipelineState::Failed;
    });

    // Manager adopts the FSM's already-minted turn id (the one
    // minted on `speech_started`) so PlaybackFinished events that
    // carry the id all the way back to the FSM match
    // `Fsm::active_.id`. Without this, FSM and Manager mint separate
    // ids for the same logical turn and FSM rejects every
    // PlaybackFinished as stale.
    stack->manager_->set_active_turn_provider([&fsm]{
        return fsm.snapshot().active_turn;
    });

    // Synchronously bump playback_active_turn the instant Manager
    // mints/adopts the turn id, before LlmStarted publishes. Closes
    // the race that previously stranded the FSM in Speaking.
    stack->manager_->set_turn_started_hook(
        [playback_active_turn](event::TurnId t) {
            playback_active_turn->store(t, std::memory_order_release);
        });

    stack->keep_alive_ = std::make_unique<supervisor::KeepAlive>(
        supervisor::KeepAlive::Options{
            .interval = std::chrono::milliseconds(
                cfg.llm.keep_alive_interval_seconds * 1000ULL),
            .should_fire = [&fsm]{
                return fsm.snapshot().state == dialogue::State::Listening;
            },
            .on_tick    = [client = stack->llm_client_.get()]{ client->keep_alive(); },
            .on_fired   = [registry]{ registry->on_keep_alive(/*fired*/ true); },
            .on_skipped = [registry]{ registry->on_keep_alive(/*fired*/ false); },
        });

    stack->manager_->start();
    stack->turn_writer_->start();
    stack->summarizer_->start();
    stack->keep_alive_->start();

    if (!stack->llm_client_->probe()) {
        log::info("main",
            "llama /health probe failed; will still attempt requests");
    }

    // Echo streamed sentences to the terminal for visual confirmation.
    // Useful in both stdin and mic-driven modes.
    subscription_keepalive.push_back(bus.subscribe<event::LlmSentence>({},
        [](const event::LlmSentence& e) {
            std::cout << "  " << e.text << "\n" << std::flush;
        }));

    // M7 — barge-in detector. Subscribed to SpeechStarted; promotes
    // those that arrive while Speaking (and past the AEC + cooldown
    // gates) into UserInterrupted events. No-op when
    // cfg.barge_in.enabled = false. The Apm pointer may be null when
    // capture is disabled, the stub APM build is in use, or the
    // pipeline runs without a loopback ring; the detector handles
    // that by treating require_aec_converged as a hard refusal-to-fire
    // unless the user has explicitly relaxed the gate.
    //
    // Constructed but NOT started here — main.cpp wires the on_fired
    // callback (which targets the playback engine's barge-in timer)
    // and then calls start(). Doing it in this order avoids a brief
    // window where SpeechStarted fires, the detector publishes
    // UserInterrupted, but the on_fired callback isn't connected yet
    // and the latency histogram misses the sample.
    stack->barge_in_ = std::make_unique<dialogue::BargeInDetector>(
        bus, fsm, apm, cfg.barge_in);

    return stack;
}

} // namespace acva::orchestrator
