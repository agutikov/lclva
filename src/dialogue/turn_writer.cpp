#include "dialogue/turn_writer.hpp"

#include "log/log.hpp"
#include "memory/memory_thread.hpp"

#include <fmt/format.h>

#include <utility>
#include <variant>

namespace acva::dialogue {

TurnWriter::TurnWriter(event::EventBus& bus, memory::MemoryThread& memory)
    : bus_(bus), memory_(memory) {}

TurnWriter::~TurnWriter() {
    stop();
}

void TurnWriter::start() {
    if (sub_) return;
    event::SubscribeOptions opts;
    opts.name = "dialogue.turn_writer";
    opts.queue_capacity = 256;
    // Block: never drop a turn-relevant event. Persistence is a
    // correctness concern, not a perf one.
    opts.policy = event::OverflowPolicy::Block;
    sub_ = bus_.subscribe_all(std::move(opts), [this](const event::Event& e) {
        on_event(e);
    });
}

void TurnWriter::stop() {
    if (!sub_) return;
    sub_->stop();
    sub_.reset();
}

void TurnWriter::on_event(const event::Event& e) {
    std::visit([this]<class T>(const T& evt) {
        if constexpr (std::is_same_v<T, event::FinalTranscript>) {
            handle_final(evt);
        } else if constexpr (std::is_same_v<T, event::LlmStarted>) {
            handle_started(evt);
        } else if constexpr (std::is_same_v<T, event::LlmSentence>) {
            handle_sentence(evt);
        } else if constexpr (std::is_same_v<T, event::LlmFinished>) {
            handle_finished(evt);
        } else if constexpr (std::is_same_v<T, event::PlaybackFinished>) {
            handle_playback_finished(evt);
        }
    }, e);
}

void TurnWriter::handle_final(const event::FinalTranscript& e) {
    const auto sid = session_.load(std::memory_order_acquire);
    if (sid == 0) return; // no session bound yet; tests use this path
    if (e.text.empty()) return;

    const auto started = memory::now_ms();
    const auto text = e.text;
    const auto lang = e.lang;

    memory_.post([sid, started, text = std::move(text), lang = std::move(lang)]
                 (memory::Repository& repo) mutable {
        auto r = repo.insert_turn(sid, memory::TurnRole::User,
                                   std::move(text),
                                   lang.empty() ? std::optional<std::string>{} : std::optional{std::move(lang)},
                                   started, memory::TurnStatus::Committed);
        if (auto* err = std::get_if<memory::DbError>(&r)) {
            log::info("turn_writer",
                fmt::format("user turn insert failed: {}", err->message));
            return;
        }
        const auto tid = std::get<memory::TurnId>(r);
        (void)repo.set_turn_status(tid, memory::TurnStatus::Committed,
                                    memory::now_ms(), std::nullopt, std::nullopt);
    });
}

void TurnWriter::handle_started(const event::LlmStarted& e) {
    in_flight_.try_emplace(e.turn, AssistantState{
        .sentences  = {},
        .played     = {},
        .lang       = {},
        .started_at = memory::now_ms(),
    });
}

void TurnWriter::handle_sentence(const event::LlmSentence& e) {
    auto& st = in_flight_[e.turn];
    if (st.lang.empty()) st.lang = e.lang;
    // Sentences arrive in seq order from Manager (it bumps a local
    // counter inside run_one), but use seq as authoritative — a
    // future speculation path may produce out-of-order sequences.
    if (e.seq >= st.sentences.size()) {
        st.sentences.resize(e.seq + 1);
        st.played.resize(e.seq + 1, false);
    }
    st.sentences[e.seq] = e.text;
}

void TurnWriter::handle_playback_finished(const event::PlaybackFinished& e) {
    auto it = in_flight_.find(e.turn);
    if (it == in_flight_.end()) return;
    auto& st = it->second;
    if (e.seq >= st.played.size()) {
        // Could happen if PlaybackFinished arrives before LlmSentence
        // (re-ordering is uncommon but the bus is multi-threaded).
        // Grow defensively so the played-flag survives.
        st.sentences.resize(e.seq + 1);
        st.played.resize(e.seq + 1, false);
    }
    st.played[e.seq] = true;
}

void TurnWriter::handle_finished(const event::LlmFinished& e) {
    auto it = in_flight_.find(e.turn);
    if (it == in_flight_.end()) return;
    AssistantState st = std::move(it->second);
    in_flight_.erase(it);

    const auto sid = session_.load(std::memory_order_acquire);
    if (sid == 0) return;

    // Build the persisted text. M7 persistence policy:
    //   - cancelled=false: full emitted text (every sentence the LLM
    //                       produced through the splitter).
    //   - cancelled=true : only sentences with played=true (what the
    //                       user actually heard), in seq order.
    std::string text;
    std::size_t persisted_count = 0;
    for (std::size_t i = 0; i < st.sentences.size(); ++i) {
        const auto& s = st.sentences[i];
        if (s.empty()) continue;
        if (e.cancelled && (i >= st.played.size() || !st.played[i])) continue;
        if (!text.empty()) text.push_back(' ');
        text.append(s);
        ++persisted_count;
    }

    // Discarded: cancelled with nothing played. No row written —
    // matches §6 of project_design.md ("Discarded: do not write the
    // assistant turn to memory").
    if (e.cancelled && persisted_count == 0) {
        log::info("turn_writer", fmt::format(
            "assistant turn {}: cancelled before any sentence played; "
            "discarding (no row written)", e.turn));
        return;
    }

    const auto status = e.cancelled ? memory::TurnStatus::Interrupted
                                    : memory::TurnStatus::Committed;
    const auto interrupted_at = e.cancelled
        ? std::optional<std::int64_t>{static_cast<std::int64_t>(persisted_count)}
        : std::nullopt;
    const auto ended = memory::now_ms();
    auto lang = std::move(st.lang);

    memory_.post([sid, text = std::move(text), lang = std::move(lang),
                  started_at = st.started_at, status, interrupted_at, ended]
                 (memory::Repository& repo) mutable {
        auto r = repo.insert_turn(sid, memory::TurnRole::Assistant,
                                   std::move(text),
                                   lang.empty() ? std::optional<std::string>{} : std::optional{std::move(lang)},
                                   started_at, status);
        if (auto* err = std::get_if<memory::DbError>(&r)) {
            log::info("turn_writer",
                fmt::format("assistant turn insert failed: {}", err->message));
            return;
        }
        const auto tid = std::get<memory::TurnId>(r);
        (void)repo.set_turn_status(tid, status, ended, interrupted_at, std::nullopt);
    });
}

} // namespace acva::dialogue
