#include "memory/recovery.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <sstream>

namespace acva::memory {

namespace {

// Cheap, deterministic, non-cryptographic hash. The hash only needs to be
// stable across builds and detect content drift; it does not need to resist
// adversarial collisions. std::hash isn't required to be stable across
// implementations, so we roll a 64-bit FNV-1a.
std::string fnv1a64(std::string_view s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ULL;
    }
    return fmt::format("{:016x}", h);
}

} // namespace

std::string compute_source_hash(Repository& repo, SessionId session,
                                 TurnId range_start, TurnId range_end) {
    auto turns_or = repo.recent_turns(session, /*limit=*/100000);
    if (auto* err = std::get_if<DbError>(&turns_or)) {
        log::warn("memory.recovery", fmt::format("hash: recent_turns failed: {}", err->message));
        return {};
    }
    const auto& turns = std::get<std::vector<TurnRow>>(turns_or);

    std::ostringstream buf;
    for (const auto& t : turns) {
        if (t.id < range_start || t.id > range_end) continue;
        buf << t.id << '|';
        if (t.text) buf << *t.text;
        buf << "\n";
    }
    return fnv1a64(buf.str());
}

Result<RecoverySummary> run_recovery(Repository& repo, Database& db) {
    RecoverySummary out;
    Database::Transaction tx(db, /*immediate=*/true);

    // 1. Close dangling sessions.
    auto open_or = repo.sessions_open_no_ended_at();
    if (auto* err = std::get_if<DbError>(&open_or)) {
        return *err;
    }
    for (const auto& s : std::get<std::vector<SessionRow>>(open_or)) {
        auto end_or = repo.max_turn_ended_at(s.id);
        if (auto* err = std::get_if<DbError>(&end_or)) {
            return *err;
        }
        const auto end_opt = std::get<std::optional<UnixMs>>(end_or);
        const UnixMs end_at = end_opt.value_or(s.started_at);
        if (auto err = repo.close_session(s.id, end_at)) {
            return *err;
        }
        ++out.sessions_closed;
    }

    // 2. Mark in-progress turns interrupted.
    auto in_prog_or = repo.turns_in_progress();
    if (auto* err = std::get_if<DbError>(&in_prog_or)) {
        return *err;
    }
    for (const auto& t : std::get<std::vector<TurnRow>>(in_prog_or)) {
        if (auto err = repo.set_turn_status(t.id, TurnStatus::Interrupted,
                                             /*ended_at=*/std::nullopt,
                                             /*interrupted_at_sentence=*/std::nullopt,
                                             /*text=*/std::nullopt)) {
            return *err;
        }
        ++out.turns_marked_interrupted;
    }

    // 3. Verify summaries.
    auto sums_or = repo.all_summaries();
    if (auto* err = std::get_if<DbError>(&sums_or)) {
        return *err;
    }
    for (const auto& s : std::get<std::vector<SummaryRow>>(sums_or)) {
        ++out.summaries_total;
        const auto current = compute_source_hash(repo, s.session_id,
                                                  s.range_start_turn,
                                                  s.range_end_turn);
        if (!current.empty() && current != s.source_hash) {
            ++out.summaries_stale;
        }
    }

    if (auto err = tx.commit()) {
        return *err;
    }
    return out;
}

} // namespace acva::memory
