#pragma once

#include "memory/db.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lclva::memory {

using SessionId = std::int64_t;
using TurnId    = std::int64_t;
using SummaryId = std::int64_t;
using FactId    = std::int64_t;

using UnixMs = std::int64_t; // milliseconds since epoch

inline UnixMs now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

enum class TurnRole : std::uint8_t { User, Assistant };
enum class TurnStatus : std::uint8_t { InProgress, Committed, Interrupted, Discarded };

[[nodiscard]] std::string_view to_string(TurnRole r) noexcept;
[[nodiscard]] std::string_view to_string(TurnStatus s) noexcept;

struct SessionRow {
    SessionId id = 0;
    UnixMs started_at = 0;
    std::optional<UnixMs> ended_at;
    std::optional<std::string> title;
};

struct TurnRow {
    TurnId id = 0;
    SessionId session_id = 0;
    TurnRole role = TurnRole::User;
    std::optional<std::string> text;
    std::optional<std::string> lang;
    UnixMs started_at = 0;
    std::optional<UnixMs> ended_at;
    TurnStatus status = TurnStatus::InProgress;
    std::optional<std::int64_t> interrupted_at_sentence;
    std::optional<std::string> audio_path;
};

struct SummaryRow {
    SummaryId id = 0;
    SessionId session_id = 0;
    TurnId range_start_turn = 0;
    TurnId range_end_turn = 0;
    std::string summary;
    std::string lang;
    std::string source_hash;
    UnixMs created_at = 0;
};

struct FactRow {
    FactId id = 0;
    std::string key;
    std::string value;
    std::optional<std::string> lang;
    std::optional<TurnId> source_turn_id;
    double confidence = 0.0;
    UnixMs updated_at = 0;
};

// Repository: typed CRUD on top of Database. All methods are synchronous and
// expect to run on the memory thread (Database is single-thread-by-policy).
class Repository {
public:
    explicit Repository(Database& db) : db_(db) {}

    [[nodiscard]] Database& database() noexcept { return db_; }

    // ----- sessions -----
    [[nodiscard]] Result<SessionId> insert_session(UnixMs started_at,
                                                    std::optional<std::string> title);
    [[nodiscard]] std::optional<DbError> close_session(SessionId id, UnixMs ended_at);
    [[nodiscard]] Result<std::vector<SessionRow>> sessions_open();
    [[nodiscard]] Result<std::vector<SessionRow>> sessions_open_no_ended_at();

    // ----- turns -----
    [[nodiscard]] Result<TurnId> insert_turn(SessionId session, TurnRole role,
                                              std::optional<std::string> text,
                                              std::optional<std::string> lang,
                                              UnixMs started_at,
                                              TurnStatus status);
    [[nodiscard]] std::optional<DbError> set_turn_status(
        TurnId id, TurnStatus status, std::optional<UnixMs> ended_at,
        std::optional<std::int64_t> interrupted_at_sentence,
        std::optional<std::string> text);
    [[nodiscard]] Result<std::vector<TurnRow>> recent_turns(SessionId session, int limit);
    [[nodiscard]] Result<std::vector<TurnRow>> turns_in_progress();
    [[nodiscard]] Result<std::optional<UnixMs>> max_turn_ended_at(SessionId session);

    // ----- summaries -----
    [[nodiscard]] Result<SummaryId> insert_summary(SessionId session,
                                                    TurnId range_start, TurnId range_end,
                                                    std::string summary,
                                                    std::string lang,
                                                    std::string source_hash,
                                                    UnixMs created_at);
    [[nodiscard]] Result<std::optional<SummaryRow>> latest_summary(SessionId session);
    [[nodiscard]] Result<std::vector<SummaryRow>> all_summaries();

    // ----- facts -----
    [[nodiscard]] std::optional<DbError> upsert_fact(std::string_view key,
                                                      std::optional<std::string_view> lang,
                                                      std::string_view value,
                                                      std::optional<TurnId> source_turn,
                                                      double confidence,
                                                      UnixMs updated_at);
    [[nodiscard]] Result<std::vector<FactRow>> facts_with_min_confidence(double min);

    // ----- settings -----
    [[nodiscard]] std::optional<DbError> set_setting(std::string_view key,
                                                      std::string_view value,
                                                      UnixMs updated_at);
    [[nodiscard]] Result<std::optional<std::string>> get_setting(std::string_view key);

private:
    Database& db_;
};

} // namespace lclva::memory
