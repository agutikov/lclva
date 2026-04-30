#include "dialogue/turn_writer.hpp"

#include "log/log.hpp"
#include "memory/memory_thread.hpp"

#include <fmt/format.h>

#include <utility>
#include <variant>

namespace lclva::dialogue {

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
        .text = {},
        .lang = {},
        .started_at = memory::now_ms(),
        .sentences = 0,
    });
}

void TurnWriter::handle_sentence(const event::LlmSentence& e) {
    auto& st = in_flight_[e.turn];
    if (st.lang.empty()) st.lang = e.lang;
    if (!st.text.empty()) st.text.push_back(' ');
    st.text.append(e.text);
    ++st.sentences;
}

void TurnWriter::handle_finished(const event::LlmFinished& e) {
    auto it = in_flight_.find(e.turn);
    if (it == in_flight_.end()) return;
    AssistantState st = std::move(it->second);
    in_flight_.erase(it);

    const auto sid = session_.load(std::memory_order_acquire);
    if (sid == 0) return;

    // Discarded: cancelled with nothing emitted. No row written.
    if (e.cancelled && st.sentences == 0) return;

    const auto status = e.cancelled ? memory::TurnStatus::Interrupted
                                    : memory::TurnStatus::Committed;
    const auto interrupted_at = e.cancelled
        ? std::optional<std::int64_t>{static_cast<std::int64_t>(st.sentences)}
        : std::nullopt;
    const auto ended = memory::now_ms();

    memory_.post([sid, st = std::move(st), status, interrupted_at, ended]
                 (memory::Repository& repo) mutable {
        auto r = repo.insert_turn(sid, memory::TurnRole::Assistant,
                                   std::move(st.text),
                                   st.lang.empty() ? std::optional<std::string>{} : std::optional{std::move(st.lang)},
                                   st.started_at, status);
        if (auto* err = std::get_if<memory::DbError>(&r)) {
            log::info("turn_writer",
                fmt::format("assistant turn insert failed: {}", err->message));
            return;
        }
        const auto tid = std::get<memory::TurnId>(r);
        (void)repo.set_turn_status(tid, status, ended, interrupted_at, std::nullopt);
    });
}

} // namespace lclva::dialogue
