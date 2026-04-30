#pragma once

#include "memory/repository.hpp"

#include <cstdint>

namespace lclva::memory {

struct RecoverySummary {
    std::uint64_t sessions_closed = 0;
    std::uint64_t turns_marked_interrupted = 0;
    std::uint64_t summaries_total = 0;
    std::uint64_t summaries_stale = 0; // source_hash mismatch
};

// Run the startup recovery sweep. Idempotent.
//
// Behaviour (project_design.md §9.2):
//   1. For each session with ended_at IS NULL: set ended_at to MAX(turns.ended_at)
//      or, if no turns have ended_at, to the session's started_at.
//   2. For each turn with status='in_progress': mark it 'interrupted', clear
//      interrupted_at_sentence.
//   3. For each summary, recompute source_hash over the source turn texts and
//      flag for refresh if it diverges. (M1 only counts; the summarizer
//      re-runs them lazily when summary triggers fire.)
//
// Runs inside a single transaction. Returns counters for logging/metrics.
[[nodiscard]] Result<RecoverySummary> run_recovery(Repository& repo, Database& db);

// Compute the source-hash of a range of turn texts. Public so the summarizer
// can re-use the same algorithm when it writes new summaries.
[[nodiscard]] std::string compute_source_hash(Repository& repo,
                                                SessionId session,
                                                TurnId range_start,
                                                TurnId range_end);

} // namespace lclva::memory
