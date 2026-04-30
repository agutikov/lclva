#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace mem = acva::memory;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("acva-repo-") + name + ".db");
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

TEST_CASE("repo: turn lifecycle (insert → status update)") {
    auto p = tmp_db("turn-lifecycle");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto sid = std::get<mem::SessionId>(repo.insert_session(mem::now_ms(), std::nullopt));

    auto tid_or = repo.insert_turn(sid, mem::TurnRole::User, std::string("hello"),
                                    std::string("en"), mem::now_ms(),
                                    mem::TurnStatus::InProgress);
    REQUIRE(std::holds_alternative<mem::TurnId>(tid_or));
    const auto tid = std::get<mem::TurnId>(tid_or);

    // List in_progress finds it.
    auto in_prog = repo.turns_in_progress();
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(in_prog));
    REQUIRE(std::get<std::vector<mem::TurnRow>>(in_prog).size() == 1);

    // Mark committed with text.
    auto err = repo.set_turn_status(tid, mem::TurnStatus::Committed, mem::now_ms(),
                                     std::nullopt, std::string("hello world"));
    CHECK_FALSE(err.has_value());

    auto recent = repo.recent_turns(sid, 10);
    REQUIRE(std::holds_alternative<std::vector<mem::TurnRow>>(recent));
    const auto& rows = std::get<std::vector<mem::TurnRow>>(recent);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].status == mem::TurnStatus::Committed);
    REQUIRE(rows[0].text.has_value());
    CHECK(*rows[0].text == "hello world");
}

TEST_CASE("repo: facts upsert is keyed on (key, lang)") {
    auto p = tmp_db("facts");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"en"}, "Alex",
                                  std::nullopt, 0.9, mem::now_ms()).has_value());
    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"ru"}, "Алекс",
                                  std::nullopt, 0.9, mem::now_ms()).has_value());

    // Upsert overwrites the same (key, lang).
    CHECK_FALSE(repo.upsert_fact("name", std::string_view{"en"}, "Alexei",
                                  std::nullopt, 0.95, mem::now_ms()).has_value());

    auto facts = repo.facts_with_min_confidence(0.5);
    REQUIRE(std::holds_alternative<std::vector<mem::FactRow>>(facts));
    const auto& fs_rows = std::get<std::vector<mem::FactRow>>(facts);
    REQUIRE(fs_rows.size() == 2);

    // Both should be present, en updated to "Alexei".
    bool found_en = false, found_ru = false;
    for (const auto& f : fs_rows) {
        if (f.lang && *f.lang == "en") {
            found_en = true;
            CHECK(f.value == "Alexei");
            CHECK(f.confidence == doctest::Approx(0.95));
        }
        if (f.lang && *f.lang == "ru") {
            found_ru = true;
        }
    }
    CHECK(found_en);
    CHECK(found_ru);
}

TEST_CASE("repo: settings get/set round-trip") {
    auto p = tmp_db("settings");
    auto db = open_or_die(p);
    mem::Repository repo(db);

    auto missing = repo.get_setting("nope");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(missing));
    CHECK_FALSE(std::get<std::optional<std::string>>(missing).has_value());

    CHECK_FALSE(repo.set_setting("k", "v1", mem::now_ms()).has_value());
    auto got = repo.get_setting("k");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(got));
    REQUIRE(std::get<std::optional<std::string>>(got).has_value());
    CHECK(*std::get<std::optional<std::string>>(got) == "v1");

    // Update.
    CHECK_FALSE(repo.set_setting("k", "v2", mem::now_ms()).has_value());
    got = repo.get_setting("k");
    CHECK(*std::get<std::optional<std::string>>(got) == "v2");
}
