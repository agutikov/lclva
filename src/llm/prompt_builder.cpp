#include "llm/prompt_builder.hpp"

#include "memory/memory_thread.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace lclva::llm {

namespace {

struct ChatMessage {
    std::string role;
    std::string content;
};

struct ChatRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    double temperature = 0.0;
    int max_tokens = 0;
    bool stream = true;
};

// Bundle of memory state read in a single round-trip on the memory thread.
struct MemorySnapshot {
    std::vector<memory::FactRow> facts;
    std::optional<memory::SummaryRow> summary;
    std::vector<memory::TurnRow> turns;  // oldest-first, already filtered
};

const std::string& choose_system_prompt(const config::Config& cfg, const std::string& lang) {
    const auto& prompts = cfg.dialogue.system_prompts;
    if (auto it = prompts.find(lang); it != prompts.end()) return it->second;
    if (auto it = prompts.find(cfg.dialogue.fallback_language); it != prompts.end()) {
        return it->second;
    }
    static const std::string empty;
    return empty;
}

std::string assemble_system_content(const config::Config& cfg,
                                    const std::string& lang,
                                    const MemorySnapshot& snap) {
    std::ostringstream os;
    os << choose_system_prompt(cfg, lang);

    if (!snap.facts.empty()) {
        os << "\n\nDurable user facts:";
        for (const auto& f : snap.facts) {
            os << "\n- " << f.key << ": " << f.value;
        }
    }
    if (snap.summary.has_value()) {
        os << "\n\nEarlier conversation summary:\n" << snap.summary->summary;
    }
    return os.str();
}

std::vector<ChatMessage> assemble_messages(std::string system_content,
                                           const MemorySnapshot& snap,
                                           std::string current_user_text) {
    std::vector<ChatMessage> msgs;
    msgs.reserve(2 + snap.turns.size());
    msgs.push_back(ChatMessage{.role = "system", .content = std::move(system_content)});
    for (const auto& t : snap.turns) {
        msgs.push_back(ChatMessage{
            .role = (t.role == memory::TurnRole::User) ? "user" : "assistant",
            .content = t.text.value_or(std::string{}),
        });
    }
    msgs.push_back(ChatMessage{.role = "user", .content = std::move(current_user_text)});
    return msgs;
}

std::size_t estimate_tokens(std::string_view body) noexcept {
    return (body.size() + 3) / 4;
}

MemorySnapshot read_snapshot(memory::MemoryThread& memory,
                             const config::Config& cfg,
                             memory::SessionId sid) {
    return memory.read([&cfg, sid](memory::Repository& repo) {
        MemorySnapshot out;

        auto facts_r = repo.facts_with_min_confidence(cfg.memory.facts.confidence_threshold);
        if (auto* v = std::get_if<std::vector<memory::FactRow>>(&facts_r)) {
            out.facts = std::move(*v);
        }

        auto sum_r = repo.latest_summary(sid);
        if (auto* v = std::get_if<std::optional<memory::SummaryRow>>(&sum_r)) {
            out.summary = std::move(*v);
        }

        auto turns_r = repo.recent_turns(sid, static_cast<int>(cfg.dialogue.recent_turns_n));
        if (auto* v = std::get_if<std::vector<memory::TurnRow>>(&turns_r)) {
            v->erase(std::remove_if(v->begin(), v->end(), [](const memory::TurnRow& t) {
                const bool usable = (t.status == memory::TurnStatus::Committed
                                  || t.status == memory::TurnStatus::Interrupted);
                return !usable || !t.text.has_value() || t.text->empty();
            }), v->end());
            out.turns = std::move(*v);
        }

        return out;
    });
}

} // namespace

PromptBuilder::PromptBuilder(const config::Config& cfg, memory::MemoryThread& memory) noexcept
    : cfg_(cfg), memory_(memory) {}

std::string PromptBuilder::build(const PromptInputs& in) {
    MemorySnapshot snap;
    if (in.session_id != 0) {
        snap = read_snapshot(memory_, cfg_, in.session_id);
    }

    std::string system_content = assemble_system_content(cfg_, in.lang, snap);
    auto messages = assemble_messages(std::move(system_content), snap, in.current_user_text);

    ChatRequest req{
        .model       = cfg_.llm.model,
        .messages    = std::move(messages),
        .temperature = cfg_.llm.temperature,
        .max_tokens  = static_cast<int>(cfg_.llm.max_tokens),
        .stream      = true,
    };

    std::string body;
    auto ec = glz::write_json(req, body);
    if (ec) body.clear();
    last_token_estimate_ = estimate_tokens(body);
    return body;
}

} // namespace lclva::llm
