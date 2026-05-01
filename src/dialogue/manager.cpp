#include "dialogue/manager.hpp"

#include "dialogue/sentence_splitter.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>
#include <variant>

namespace acva::dialogue {

Manager::Manager(const config::Config& cfg,
                 event::EventBus& bus,
                 llm::PromptBuilder& prompt_builder,
                 llm::LlmClient& client,
                 TurnFactory& turns)
    : cfg_(cfg),
      bus_(bus),
      prompt_builder_(prompt_builder),
      client_(client),
      turns_(turns) {}

Manager::~Manager() {
    stop();
}

void Manager::start() {
    if (sub_) return; // already started

    {
        std::lock_guard lk(io_mu_);
        stopping_ = false;
    }
    io_ = std::thread([this]{ io_loop(); });

    event::SubscribeOptions opts;
    opts.name = "dialogue.manager";
    opts.queue_capacity = 256;
    // Block: we'd rather pause the bus than lose a FinalTranscript or
    // CancelGeneration. The handlers are non-blocking so this is safe.
    opts.policy = event::OverflowPolicy::Block;
    sub_ = bus_.subscribe_all(std::move(opts), [this](const event::Event& e) {
        on_event(e);
    });
}

void Manager::stop() {
    if (sub_) {
        sub_->stop();
        sub_.reset();
    }
    if (io_.joinable()) {
        cancel_active(event::kNoTurn);   // unblock any in-flight submit()
        {
            std::lock_guard lk(io_mu_);
            stopping_ = true;
        }
        io_cv_.notify_all();
        io_.join();
    }
}

void Manager::set_pipeline_gate(PipelineGate gate) noexcept {
    std::lock_guard lk(gate_mu_);
    gate_ = std::move(gate);
}

bool Manager::gate_open() const {
    PipelineGate g;
    {
        std::lock_guard lk(gate_mu_);
        g = gate_;
    }
    if (!g) return true;       // no gate installed → always open
    try {
        return g();
    } catch (...) {
        // Defensive: if the gate predicate throws, default to "open"
        // so a buggy supervisor wiring can't permanently freeze the
        // dialogue path. The exception is logged once.
        log::info("dialogue", "pipeline gate predicate threw; treating as open");
        return true;
    }
}

void Manager::on_event(const event::Event& e) {
    std::visit([this]<class T>(const T& evt) {
        if constexpr (std::is_same_v<T, event::FinalTranscript>) {
            if (!gate_open()) {
                gated_turns_.fetch_add(1, std::memory_order_relaxed);
                // Rate-limit the log line at once per minute. Without this
                // a stuck-Unhealthy backend would dominate the log when a
                // user keeps typing into stdin / speaking into the mic.
                bool should_log = false;
                {
                    std::lock_guard lk(gate_mu_);
                    const auto now = std::chrono::steady_clock::now();
                    if (gate_last_log_at_ == std::chrono::steady_clock::time_point{}
                        || now - gate_last_log_at_ >= std::chrono::seconds(60)) {
                        gate_last_log_at_ = now;
                        should_log = true;
                    }
                }
                if (should_log) {
                    log::info("dialogue",
                        fmt::format(
                            "pipeline gated; refusing FinalTranscript turn={} "
                            "(supervisor reports a critical backend unhealthy)",
                            evt.turn));
                }
                // Publish UserInterrupted so the FSM (which advances to
                // Thinking on FinalTranscript when in Transcribing) is
                // returned to Listening with a Discarded outcome. The
                // event applies to the FSM's active turn — the field
                // value here is informational.
                bus_.publish(event::UserInterrupted{
                    .turn = evt.turn,
                    .ts   = std::chrono::steady_clock::now(),
                });
                return;
            }
            enqueue_turn(evt);
        } else if constexpr (std::is_same_v<T, event::UserInterrupted>) {
            cancel_active(evt.turn);
        } else if constexpr (std::is_same_v<T, event::CancelGeneration>) {
            cancel_active(evt.turn);
        }
    }, e);
}

void Manager::cancel_active(event::TurnId target) {
    std::lock_guard lk(active_mu_);
    if (!active_.valid()) return;
    if (target != event::kNoTurn && target != active_.id) return;
    if (active_.token) active_.token->cancel();
}

void Manager::enqueue_turn(event::FinalTranscript e) {
    {
        std::lock_guard lk(io_mu_);
        if (pending_) {
            // M1 keeps the I/O thread serial. Replacing the pending slot
            // means the previous request was superseded before it ran —
            // log it and move on.
            log::info("dialogue",
                fmt::format("dropping superseded transcript turn={}",
                            pending_->turn));
        }
        pending_ = std::move(e);
    }
    io_cv_.notify_one();
}

void Manager::io_loop() {
    while (true) {
        event::FinalTranscript job;
        {
            std::unique_lock lk(io_mu_);
            io_cv_.wait(lk, [this]{ return stopping_ || pending_.has_value(); });
            if (stopping_ && !pending_) return;
            if (!pending_) continue;
            job = std::move(*pending_);
            pending_.reset();
        }
        run_one(job);
    }
}

void Manager::run_one(const event::FinalTranscript& e) {
    TurnContext ctx = turns_.mint();
    {
        std::lock_guard lk(active_mu_);
        active_ = ctx;
    }

    auto lang = e.lang.empty() ? cfg_.dialogue.fallback_language : e.lang;

    auto body = prompt_builder_.build({
        .session_id        = session_.load(std::memory_order_acquire),
        .lang              = lang,
        .current_user_text = e.text,
    });

    bus_.publish(event::LlmStarted{ .turn = ctx.id });

    SentenceSplitter splitter(cfg_.dialogue.sentence_splitter);
    std::vector<std::string> emitted;
    event::SequenceNo seq = 0;
    const auto cap = cfg_.dialogue.max_assistant_sentences;
    bool cap_reached = false;

    auto enforce_cap = [&] {
        // Once the cap is hit, cancel the LLM stream so libcurl's
        // write callback aborts on the next chunk. Already-emitted
        // sentences keep flowing through the TTS bridge unchanged.
        if (!cap_reached && cap > 0 && seq >= cap) {
            cap_reached = true;
            if (ctx.token) ctx.token->cancel();
            log::info("dialogue", fmt::format(
                "sentence cap hit: turn={} cap={} — cancelling LLM stream",
                ctx.id, cap));
        }
    };

    auto on_token = [&](std::string_view delta) {
        if (cap_reached) return;
        emitted.clear();
        splitter.push(delta, emitted);
        for (auto& s : emitted) {
            if (cap > 0 && seq >= cap) break;
            bus_.publish(event::LlmSentence{
                .turn = ctx.id,
                .seq  = seq++,
                .text = std::move(s),
                .lang = lang,
            });
        }
        enforce_cap();
        if (!cap_reached) {
            bus_.publish(event::LlmToken{ .turn = ctx.id, .token = std::string{delta} });
        }
    };

    auto on_finished = [&](llm::LlmFinish f) {
        emitted.clear();
        splitter.flush(emitted);
        for (auto& s : emitted) {
            if (cap > 0 && seq >= cap) break;
            bus_.publish(event::LlmSentence{
                .turn = ctx.id,
                .seq  = seq++,
                .text = std::move(s),
                .lang = lang,
            });
        }
        if (f.error) {
            log::info("dialogue",
                fmt::format("llm error turn={} msg={}", ctx.id, f.error_message));
        }
    };

    client_.submit(llm::LlmRequest{
        .body   = std::move(body),
        .cancel = ctx.token,
        .turn   = ctx.id,
        .lang   = lang,
    }, llm::LlmCallbacks{
        .on_token    = on_token,
        .on_finished = on_finished,
    });

    {
        std::lock_guard lk(active_mu_);
        active_ = TurnContext{};
    }
}

} // namespace acva::dialogue
