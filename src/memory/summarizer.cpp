#include "memory/summarizer.hpp"

#include "dialogue/turn.hpp"
#include "llm/client.hpp"
#include "log/log.hpp"
#include "memory/memory_thread.hpp"
#include "memory/recovery.hpp"

#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <sstream>
#include <utility>
#include <variant>

namespace lclva::memory {

namespace {

// Glaze reflection requires types to live at namespace scope.
struct SummaryMessage { std::string role; std::string content; };
struct SummaryRequest {
    std::string model;
    std::vector<SummaryMessage> messages;
    double temperature = 0.0;
    int max_tokens = 0;
    bool stream = false;
};

constexpr std::string_view kSystemPrompt =
    "You summarize conversations. Produce a single short paragraph "
    "(<=2 sentences, <=40 words) capturing the main topics, decisions, "
    "and any durable user preferences mentioned. Do not quote verbatim.";

std::string concat_turns(const std::vector<TurnRow>& turns,
                         TurnId range_start, TurnId range_end) {
    std::ostringstream os;
    for (const auto& t : turns) {
        if (t.id < range_start || t.id > range_end) continue;
        if (!t.text.has_value() || t.text->empty()) continue;
        os << (t.role == TurnRole::User ? "User: " : "Assistant: ")
           << *t.text << "\n";
    }
    return os.str();
}

std::string build_request_body(const config::Config& cfg, const std::string& text_block) {
    SummaryRequest req{
        .model = cfg.llm.model,
        .messages = {
            SummaryMessage{.role = "system", .content = std::string{kSystemPrompt}},
            SummaryMessage{.role = "user",   .content = "Summarize:\n" + text_block},
        },
        .temperature = 0.3,
        .max_tokens = 200,
        .stream = true,
    };
    std::string out;
    auto ec = glz::write_json(req, out);
    if (ec) out.clear();
    return out;
}

} // namespace

Summarizer::Summarizer(const config::Config& cfg,
                       event::EventBus& bus,
                       MemoryThread& memory,
                       llm::LlmClient& client)
    : cfg_(cfg), bus_(bus), memory_(memory), client_(client) {}

Summarizer::~Summarizer() {
    stop();
}

void Summarizer::start() {
    if (sub_) return;
    {
        std::lock_guard lk(worker_mu_);
        stopping_ = false;
    }
    worker_ = std::thread([this]{ worker_loop(); });

    event::SubscribeOptions opts;
    opts.name = "memory.summarizer";
    opts.queue_capacity = 64;
    opts.policy = event::OverflowPolicy::DropOldest;
    sub_ = bus_.subscribe<event::LlmFinished>(std::move(opts),
        [this](const event::LlmFinished& e) { on_event(event::Event{e}); });
}

void Summarizer::stop() {
    if (sub_) {
        sub_->stop();
        sub_.reset();
    }
    if (worker_.joinable()) {
        {
            std::lock_guard lk(worker_mu_);
            stopping_ = true;
        }
        worker_cv_.notify_all();
        worker_.join();
    }
}

void Summarizer::on_event(const event::Event& e) {
    std::visit([this]<class T>(const T& evt) {
        if constexpr (std::is_same_v<T, event::LlmFinished>) {
            if (evt.cancelled) return;
            // Each completed exchange persists ~2 rows (user + assistant).
            const auto count = turns_since_summary_.fetch_add(2,
                                    std::memory_order_acq_rel) + 2;
            if (count >= cfg_.memory.summary.turn_threshold) {
                trigger_now();
            }
        }
    }, e);
}

void Summarizer::trigger_now() {
    {
        std::lock_guard lk(worker_mu_);
        job_pending_ = true;
    }
    worker_cv_.notify_one();
}

void Summarizer::worker_loop() {
    while (true) {
        {
            std::unique_lock lk(worker_mu_);
            worker_cv_.wait(lk, [this]{ return stopping_ || job_pending_; });
            if (stopping_ && !job_pending_) return;
            job_pending_ = false;
        }
        const bool wrote = run_one_summary();
        if (wrote) {
            turns_since_summary_.store(0, std::memory_order_release);
            summaries_written_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}

bool Summarizer::run_one_summary() {
    const auto sid = session_.load(std::memory_order_acquire);
    if (sid == 0) return false;

    // Read the range to summarize: from (latest_summary.range_end + 1) to
    // (max committed turn). The memory thread runs both reads serially,
    // so the snapshot is internally consistent.
    struct Snapshot {
        TurnId range_start = 0;
        TurnId range_end = 0;
        std::vector<TurnRow> turns;
    };
    auto snap = memory_.read([sid](Repository& repo) {
        Snapshot s;
        auto sum_r = repo.latest_summary(sid);
        if (auto* opt = std::get_if<std::optional<SummaryRow>>(&sum_r);
            opt && opt->has_value()) {
            s.range_start = (*opt)->range_end_turn + 1;
        } else {
            s.range_start = 1;
        }
        auto turns_r = repo.recent_turns(sid, /*limit=*/100000);
        if (auto* v = std::get_if<std::vector<TurnRow>>(&turns_r)) {
            s.turns = std::move(*v);
        }
        // Use the largest in-range turn id as range_end.
        for (const auto& t : s.turns) {
            if (t.id >= s.range_start
                && (t.status == TurnStatus::Committed
                 || t.status == TurnStatus::Interrupted)) {
                if (t.id > s.range_end) s.range_end = t.id;
            }
        }
        return s;
    });

    if (snap.range_end < snap.range_start || snap.turns.empty()) {
        return false; // nothing new
    }

    auto block = concat_turns(snap.turns, snap.range_start, snap.range_end);
    if (block.empty()) return false;

    auto body = build_request_body(cfg_, block);
    if (body.empty()) {
        log::info("memory.summarizer", "request body build failed");
        return false;
    }

    // Synchronous LLM call. We collect tokens into a single string.
    std::string summary_text;
    auto cancel = std::make_shared<dialogue::CancellationToken>();
    bool errored = false;

    client_.submit(llm::LlmRequest{
        .body   = std::move(body),
        .cancel = cancel,
        .turn   = event::kNoTurn,         // not a dialogue turn
        .lang   = "en",
    }, llm::LlmCallbacks{
        .on_token = [&](std::string_view t) { summary_text.append(t); },
        .on_finished = [&](llm::LlmFinish f) {
            if (f.error) errored = true;
        },
    });

    if (errored || summary_text.empty()) {
        log::info("memory.summarizer",
            "summary llm call failed; recording placeholder");
        summary_text = "[TODO summary]";
    }

    const auto rs = snap.range_start;
    const auto re = snap.range_end;
    auto fut = memory_.submit([sid, rs, re,
                               text = std::move(summary_text)]
                              (Repository& repo) mutable -> bool {
        auto hash = compute_source_hash(repo, sid, rs, re);
        auto r = repo.insert_summary(sid, rs, re, std::move(text),
                                      "en", std::move(hash), now_ms());
        if (auto* err = std::get_if<DbError>(&r)) {
            log::info("memory.summarizer",
                fmt::format("insert_summary failed: {}", err->message));
            return false;
        }
        return true;
    });

    return fut.get();
}

} // namespace lclva::memory
