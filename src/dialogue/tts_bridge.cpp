#include "dialogue/tts_bridge.hpp"

#include "audio/resampler.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>
#include <variant>

namespace acva::dialogue {

TtsBridge::TtsBridge(const config::Config& cfg,
                      event::EventBus& bus,
                      SubmitFn submit_fn,
                      playback::PlaybackQueue& queue)
    : cfg_(cfg), bus_(bus),
      submit_fn_(std::move(submit_fn)), queue_(queue) {}

TtsBridge::~TtsBridge() {
    stop();
}

void TtsBridge::start() {
    if (sub_) return;
    {
        std::lock_guard lk(mu_);
        stopping_ = false;
    }
    io_ = std::thread([this]{ io_loop(); });

    event::SubscribeOptions opts;
    opts.name = "dialogue.tts_bridge";
    opts.queue_capacity = 256;
    opts.policy = event::OverflowPolicy::Block;   // never lose a sentence
    sub_ = bus_.subscribe_all(std::move(opts), [this](const event::Event& e) {
        on_event(e);
    });
}

void TtsBridge::stop() {
    if (sub_) {
        sub_->stop();
        sub_.reset();
    }
    if (io_.joinable()) {
        // Cancel anything currently in flight so submit() returns
        // promptly, then signal the loop to drain.
        {
            std::lock_guard lk(mu_);
            if (in_flight_) in_flight_->cancel();
            stopping_ = true;
        }
        cv_.notify_all();
        io_.join();
    }
}

std::size_t TtsBridge::pending() const {
    std::lock_guard lk(mu_);
    return pending_.size();
}

std::shared_ptr<CancellationToken> TtsBridge::token_for(event::TurnId turn) {
    auto it = tokens_.find(turn);
    if (it == tokens_.end()) {
        // No LlmStarted observed yet (race or test harness emitting
        // bare LlmSentence). Mint one lazily so cancellation still
        // works for this turn.
        auto tok = std::make_shared<CancellationToken>();
        tokens_.emplace(turn, tok);
        return tok;
    }
    return it->second;
}

void TtsBridge::on_event(const event::Event& e) {
    std::visit([this]<class T>(const T& evt) {
        if constexpr (std::is_same_v<T, event::LlmStarted>) {
            on_llm_started(evt.turn);
        } else if constexpr (std::is_same_v<T, event::LlmSentence>) {
            on_llm_sentence(evt);
        } else if constexpr (std::is_same_v<T, event::LlmFinished>) {
            on_llm_finished(evt);
        } else if constexpr (std::is_same_v<T, event::UserInterrupted>) {
            on_user_interrupted(evt);
        }
    }, e);
}

void TtsBridge::on_llm_started(event::TurnId turn) {
    std::lock_guard lk(mu_);
    // Replace any existing token for this turn id (shouldn't happen in
    // practice but stays safe under churn).
    tokens_[turn] = std::make_shared<CancellationToken>();
}

void TtsBridge::on_llm_sentence(event::LlmSentence sentence) {
    std::shared_ptr<CancellationToken> tok;
    {
        std::lock_guard lk(mu_);
        tok = token_for(sentence.turn);
        // If the turn has already been cancelled, drop the sentence —
        // there's no point synthesizing audio that can never be
        // played.
        if (tok->is_cancelled()) {
            cancelled_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pending_.push_back(Job{
            .sentence = std::move(sentence),
            .cancel   = std::move(tok),
        });
    }
    cv_.notify_one();
}

void TtsBridge::on_llm_finished(const event::LlmFinished& e) {
    std::lock_guard lk(mu_);
    // Don't cancel here — already-emitted sentences should keep
    // synthesizing and playing. Just drop the token entry; future
    // sentences with this turn id will lazily re-mint via token_for.
    tokens_.erase(e.turn);
}

void TtsBridge::on_user_interrupted(const event::UserInterrupted& e) {
    std::vector<std::shared_ptr<CancellationToken>> to_cancel;
    {
        std::lock_guard lk(mu_);
        // Cancel the interrupted turn's token but leave it in the map
        // so any LlmSentence event that arrives later (race with the
        // bus dispatch order) finds the *same* cancelled token via
        // token_for() and skips synthesis. on_llm_finished prunes the
        // entry when the LLM stream eventually winds down.
        if (auto it = tokens_.find(e.turn); it != tokens_.end()) {
            to_cancel.push_back(it->second);
        } else {
            // No LlmStarted seen yet — install a pre-cancelled token
            // so a stray LlmSentence for this turn still drops cleanly.
            auto tok = std::make_shared<CancellationToken>();
            tok->cancel();
            tokens_.emplace(e.turn, tok);
        }
        // Drop pending sentences for the cancelled turn.
        std::deque<Job> kept;
        for (auto& j : pending_) {
            if (j.sentence.turn == e.turn) {
                cancelled_.fetch_add(1, std::memory_order_relaxed);
            } else {
                kept.push_back(std::move(j));
            }
        }
        pending_ = std::move(kept);
        // Cancel anything currently being submitted to Piper.
        if (in_flight_) to_cancel.push_back(in_flight_);
    }
    for (auto& t : to_cancel) t->cancel();

    // Drain the playback queue of stale audio. Using clear() is
    // simpler than invalidate_before(turn+1) and matches the M3 plan:
    // a barge-in unconditionally silences the assistant.
    queue_.clear();
}

void TtsBridge::io_loop() {
    while (true) {
        Job job;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [this]{ return stopping_ || !pending_.empty(); });
            if (stopping_ && pending_.empty()) return;
            if (pending_.empty()) continue;
            job = std::move(pending_.front());
            pending_.pop_front();
            in_flight_ = job.cancel;
        }
        run_one(std::move(job));
        {
            std::lock_guard lk(mu_);
            in_flight_.reset();
        }
    }
}

void TtsBridge::run_one(Job job) {
    if (job.cancel && job.cancel->is_cancelled()) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    bus_.publish(event::TtsStarted{
        .turn = job.sentence.turn,
        .seq  = job.sentence.seq,
    });

    // Resampler is per-sentence: Piper voices have voice-dependent
    // sample rates (16/22.05/24 kHz), and on_format reports it back to
    // us before any audio. We construct the resampler inside
    // on_format and tear it down when the request ends. unique_ptr
    // because Resampler is non-copyable.
    std::unique_ptr<audio::Resampler> resampler;
    bool got_format = false;
    bool got_error = false;
    std::string err_msg;
    const auto device_rate =
        static_cast<double>(cfg_.audio.sample_rate_hz);

    tts::TtsCallbacks cb;
    cb.on_format = [&](int rate) {
        if (rate <= 0) {
            got_error = true;
            err_msg = "piper: invalid sample rate " + std::to_string(rate);
            return;
        }
        if (static_cast<std::uint32_t>(rate) == cfg_.audio.sample_rate_hz) {
            // Pass-through: skip the resampler entirely. Saves a
            // copy per chunk and avoids any phase wobble at common
            // matching rates (24 kHz Piper voices on a 24 kHz device).
            resampler.reset();
        } else {
            resampler = std::make_unique<audio::Resampler>(
                static_cast<double>(rate), device_rate,
                audio::Resampler::Quality::High);
        }
        got_format = true;
    };
    cb.on_audio = [&](std::span<const std::int16_t> samples) {
        if (got_error || job.cancel->is_cancelled()) return;

        std::vector<std::int16_t> out;
        if (resampler) {
            out = resampler->process(samples);
        } else {
            out.assign(samples.begin(), samples.end());
        }
        if (out.empty()) return;

        playback::AudioChunk chunk{
            .turn    = job.sentence.turn,
            .seq     = job.sentence.seq,
            .samples = std::move(out),
        };
        const auto bytes = chunk.samples.size() * sizeof(std::int16_t);
        if (queue_.enqueue(std::move(chunk))) {
            bus_.publish(event::TtsAudioChunk{
                .turn  = job.sentence.turn,
                .seq   = job.sentence.seq,
                .bytes = bytes,
            });
        }
        // Capacity overflow is counted in queue_.drops(); upstream
        // backpressure (max_tts_queue_sentences) is the right place
        // to react.
    };
    cb.on_finished = [] {};
    cb.on_error = [&](std::string m) {
        got_error = true;
        err_msg = std::move(m);
    };

    submit_fn_(tts::TtsRequest{
        .turn   = job.sentence.turn,
        .seq    = job.sentence.seq,
        .text   = job.sentence.text,
        .lang   = job.sentence.lang,
        .cancel = job.cancel,
    }, cb);

    // Flush any tail samples left inside the resampler. If the request
    // errored / was cancelled we still flush — the few samples don't
    // matter and the queue.clear() on cancel takes care of them.
    if (resampler && got_format) {
        try {
            auto tail = resampler->flush();
            if (!tail.empty() && !job.cancel->is_cancelled()) {
                const auto bytes = tail.size() * sizeof(std::int16_t);
                playback::AudioChunk chunk{
                    .turn    = job.sentence.turn,
                    .seq     = job.sentence.seq,
                    .samples = std::move(tail),
                };
                if (queue_.enqueue(std::move(chunk))) {
                    bus_.publish(event::TtsAudioChunk{
                        .turn  = job.sentence.turn,
                        .seq   = job.sentence.seq,
                        .bytes = bytes,
                    });
                }
            }
        } catch (const std::exception& ex) {
            log::info("tts_bridge",
                fmt::format("resampler flush failed turn={} seq={} err={}",
                             job.sentence.turn, job.sentence.seq, ex.what()));
        }
    }

    if (job.cancel->is_cancelled()) {
        cancelled_.fetch_add(1, std::memory_order_relaxed);
        // No TtsFinished publish — mirrors LlmFinished{cancelled=true}.
        return;
    }
    if (got_error) {
        errored_.fetch_add(1, std::memory_order_relaxed);
        log::info("tts_bridge", fmt::format(
            "synth failed turn={} seq={} lang={} err={}",
            job.sentence.turn, job.sentence.seq, job.sentence.lang, err_msg));
        return;
    }
    synthesized_.fetch_add(1, std::memory_order_relaxed);
    bus_.publish(event::TtsFinished{
        .turn = job.sentence.turn,
        .seq  = job.sentence.seq,
    });
}

} // namespace acva::dialogue
