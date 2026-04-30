#include "memory/db.hpp"
#include "memory/recovery.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace mem = lclva::memory;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("lclva-recov-") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

mem::Database open_or_die(const fs::path& p) {
    auto r = mem::Database::open(p);
    REQUIRE(std::holds_alternative<mem::Database>(r));
    return std::move(std::get<mem::Database>(r));
}

} // namespace

TEST_CASE("recovery: closes dangling sessions and marks in-progress turns interrupted") {
    auto p = tmp_db("dangling");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    // Pre-populate: a session with no ended_at, two turns (one committed, one in_progress).
    auto sid = std::get<mem::SessionId>(repo.insert_session(1000, "t"));

    auto t1 = std::get<mem::TurnId>(
        repo.insert_turn(sid, mem::TurnRole::User, std::string("hi"),
                          std::string("en"), 1000, mem::TurnStatus::Committed));
    REQUIRE(repo.set_turn_status(t1, mem::TurnStatus::Committed, /*ended_at=*/2000,
                                  std::nullopt, std::nullopt).has_value() == false);

    (void)std::get<mem::TurnId>(
        repo.insert_turn(sid, mem::TurnRole::Assistant, std::nullopt,
                          std::string("en"), 2500, mem::TurnStatus::InProgress));

    // Run recovery.
    auto sweep = mem::run_recovery(repo, db);
    REQUIRE(std::holds_alternative<mem::RecoverySummary>(sweep));
    const auto s = std::get<mem::RecoverySummary>(sweep);
    CHECK(s.sessions_closed == 1);
    CHECK(s.turns_marked_interrupted == 1);
    CHECK(s.summaries_total == 0);
    CHECK(s.summaries_stale == 0);

    // Verify post-conditions.
    auto sessions = repo.sessions_open_no_ended_at();
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(sessions));
    CHECK(std::get<std::vector<mem::SessionRow>>(sessions).empty());

    auto in_prog = repo.turns_in_progress();
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(in_prog));
    CHECK(std::get<std::vector<mem::TurnRow>>(in_prog).empty());
}

TEST_CASE("recovery: idempotent (running twice produces the same state)") {
    auto p = tmp_db("idempotent");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(1000, "t"));
    (void)std::get<mem::TurnId>(
        repo.insert_turn(sid, mem::TurnRole::User, std::string("x"),
                          std::string("en"), 1000, mem::TurnStatus::InProgress));

    auto first = mem::run_recovery(repo, db);
    REQUIRE(std::holds_alternative<mem::RecoverySummary>(first));
    auto first_s = std::get<mem::RecoverySummary>(first);
    CHECK(first_s.sessions_closed == 1);
    CHECK(first_s.turns_marked_interrupted == 1);

    auto second = mem::run_recovery(repo, db);
    REQUIRE(std::holds_alternative<mem::RecoverySummary>(second));
    auto second_s = std::get<mem::RecoverySummary>(second);
    // Nothing left to fix.
    CHECK(second_s.sessions_closed == 0);
    CHECK(second_s.turns_marked_interrupted == 0);
}

TEST_CASE("recovery: detects stale summary via source-hash mismatch") {
    auto p = tmp_db("stale-summary");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(1000, std::nullopt));
    auto t1 = std::get<mem::TurnId>(
        repo.insert_turn(sid, mem::TurnRole::User, std::string("a"),
                          std::string("en"), 1000, mem::TurnStatus::Committed));
    auto t2 = std::get<mem::TurnId>(
        repo.insert_turn(sid, mem::TurnRole::Assistant, std::string("b"),
                          std::string("en"), 1100, mem::TurnStatus::Committed));

    // Insert a summary with a deliberately wrong hash.
    auto sm = repo.insert_summary(sid, t1, t2, "summary text", "en",
                                   "0000000000000000", mem::now_ms());
    REQUIRE(std::holds_alternative<mem::SummaryId>(sm));

    auto sweep = mem::run_recovery(repo, db);
    REQUIRE(std::holds_alternative<mem::RecoverySummary>(sweep));
    auto s = std::get<mem::RecoverySummary>(sweep);
    CHECK(s.summaries_total == 1);
    CHECK(s.summaries_stale == 1);
}
