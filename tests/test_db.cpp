#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace mem = lclva::memory;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("lclva-test-") + name + ".db");
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

TEST_CASE("db: open creates schema and is idempotent") {
    auto p = tmp_db("open-idempotent");

    {
        auto db = open_or_die(p);
        mem::Repository repo(db);
        auto sid_or = repo.insert_session(mem::now_ms(), "test");
        REQUIRE(std::holds_alternative<mem::SessionId>(sid_or));
        CHECK(std::get<mem::SessionId>(sid_or) > 0);
    }

    // Reopen — schema already there, no error.
    auto db = open_or_die(p);
    mem::Repository repo(db);
    auto sessions = repo.sessions_open_no_ended_at();
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(sessions));
    CHECK(std::get<std::vector<mem::SessionRow>>(sessions).size() == 1);
}

TEST_CASE("db: pragmas applied (WAL mode)") {
    auto p = tmp_db("pragma-wal");
    auto db = open_or_die(p);

    // Verify WAL file exists after a write.
    mem::Repository repo(db);
    auto sid = repo.insert_session(mem::now_ms(), std::nullopt);
    REQUIRE(std::holds_alternative<mem::SessionId>(sid));

    auto wal = fs::path(p.string() + "-wal");
    CHECK(fs::exists(wal));
}

TEST_CASE("db: transaction commit + rollback") {
    auto p = tmp_db("txn");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    // Commit path.
    {
        mem::Database::Transaction tx(db, /*immediate=*/true);
        auto sid = repo.insert_session(mem::now_ms(), "committed");
        REQUIRE(std::holds_alternative<mem::SessionId>(sid));
        auto err = tx.commit();
        CHECK_FALSE(err.has_value());
    }

    // Rollback path.
    {
        mem::Database::Transaction tx(db, true);
        (void)repo.insert_session(mem::now_ms(), "rolled-back");
        tx.rollback();
    }

    auto rows = repo.sessions_open_no_ended_at();
    REQUIRE(std::holds_alternative<std::vector<mem::SessionRow>>(rows));
    const auto& rs = std::get<std::vector<mem::SessionRow>>(rows);
    REQUIRE(rs.size() == 1);
    REQUIRE(rs[0].title.has_value());
    CHECK(*rs[0].title == "committed");
}
