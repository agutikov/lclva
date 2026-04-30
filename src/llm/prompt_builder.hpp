#pragma once

#include "config/config.hpp"
#include "memory/repository.hpp"

#include <cstddef>
#include <string>

namespace lclva::memory { class MemoryThread; }

namespace lclva::llm {

struct PromptInputs {
    // 0 means "no session yet" — facts/summary/recent-turns reads are
    // skipped, useful for the very first turn and for tests that want to
    // exercise the assembly path without a populated DB.
    memory::SessionId session_id = 0;
    std::string lang;                // detected language for this turn
    std::string current_user_text;
};

// Assembles the OpenAI-compatible chat-completions request body from the
// memory layer + dialogue config (plans/project_design.md §9.3).
//
//   [system policy]            ← cfg.dialogue.system_prompts[lang]
//   [durable facts]            ← facts where confidence ≥ threshold
//   [cached session summary]   ← latest summaries row
//   [last N turns verbatim]    ← committed/interrupted turns, oldest-first
//   [current user turn]
//
// All memory state is read in a single round-trip on the memory thread.
class PromptBuilder {
public:
    PromptBuilder(const config::Config& cfg, memory::MemoryThread& memory) noexcept;

    // Returns the JSON body to POST at /v1/chat/completions.
    [[nodiscard]] std::string build(const PromptInputs& in);

    // Token-count estimate for the most recent build() output.
    // 1 token ≈ 4 chars (BPE rule of thumb; replace with a real tokenizer
    // if it proves too loose against the cfg.llm.max_prompt_tokens cap).
    [[nodiscard]] std::size_t last_token_estimate() const noexcept {
        return last_token_estimate_;
    }

private:
    const config::Config& cfg_;
    memory::MemoryThread& memory_;
    std::size_t last_token_estimate_ = 0;
};

} // namespace lclva::llm
