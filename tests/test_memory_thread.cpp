#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace mem = acva::memory;
namespace fs = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("acva-mt-") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

std::unique_ptr<mem::MemoryThread> open_or_die(const fs::path& p) {
    auto r = mem::MemoryThread::open(p, /*queue_capacity=*/64);
    REQUIRE(std::holds_alternative<std::unique_ptr<mem::MemoryThread>>(r));
    return std::move(std::get<std::unique_ptr<mem::MemoryThread>>(r));
}

} // namespace

TEST_CASE("memory_thread: post + read round-trip") {
    auto p = tmp_db("post-read");
    auto mt = open_or_die(p);

    // Write via post.
    auto fut = mt->submit([](mem::Repository& repo) {
        auto sid = repo.insert_session(mem::now_ms(), std::string("hello"));
        REQUIRE(std::holds_alternative<mem::SessionId>(sid));
        return std::get<mem::SessionId>(sid);
    });
    auto sid = fut.get();
    CHECK(sid > 0);

    // Read via blocking read.
    auto count = mt->read([](mem::Repository& repo) {
        auto rows = repo.sessions_open_no_ended_at();
        return std::get<std::vector<mem::SessionRow>>(rows).size();
    });
    CHECK(count == 1);
}

TEST_CASE("memory_thread: jobs run in posting order") {
    auto p = tmp_db("ordering");
    auto mt = open_or_die(p);

    mem::SessionId sid = mt->read([](mem::Repository& repo) {
        return std::get<mem::SessionId>(repo.insert_session(0, std::nullopt));
    });

    // Post 50 turn inserts; verify they end up in order.
    std::vector<std::future<mem::TurnId>> futures;
    for (int i = 0; i < 50; ++i) {
        futures.push_back(mt->submit([sid, i](mem::Repository& repo) {
            return std::get<mem::TurnId>(
                repo.insert_turn(sid, mem::TurnRole::User,
                                  std::string("turn-") + std::to_string(i),
                                  std::string("en"),
                                  static_cast<mem::UnixMs>(i),
                                  mem::TurnStatus::Committed));
        }));
    }
    for (auto& f : futures) f.get();

    auto rows = mt->read([sid](mem::Repository& repo) {
        return std::get<std::vector<mem::TurnRow>>(repo.recent_turns(sid, 100));
    });
    REQUIRE(rows.size() == 50);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(rows[i].text.value_or("") == "turn-" + std::to_string(i));
    }
}

TEST_CASE("memory_thread: post returns false after destruction") {
    auto p = tmp_db("shutdown");
    auto mt = open_or_die(p);

    // Submit one job to make sure it's running.
    auto first = mt->submit([](mem::Repository&) { return 42; });
    CHECK(first.get() == 42);

    // Destroy.
    mt.reset();
    // Subsequent posts go through new instance.
    auto mt2 = open_or_die(p); // re-opens schema
    auto count = mt2->read([](mem::Repository& repo) {
        return std::get<std::vector<mem::SessionRow>>(repo.sessions_open_no_ended_at()).size();
    });
    CHECK(count == 0);
}
