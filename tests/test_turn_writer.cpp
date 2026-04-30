#include "dialogue/turn_writer.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace dlg = acva::dialogue;
namespace ev  = acva::event;
namespace mem = acva::memory;
namespace fs  = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("acva-tw-") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

std::unique_ptr<mem::MemoryThread> open_mt(const fs::path& p) {
    auto r = mem::MemoryThread::open(p, 16);
    REQUIRE(std::holds_alternative<std::unique_ptr<mem::MemoryThread>>(r));
    return std::move(std::get<std::unique_ptr<mem::MemoryThread>>(r));
}

mem::SessionId seed_session(mem::MemoryThread& mt) {
    return mt.read([](mem::Repository& repo) {
        auto r = repo.insert_session(mem::now_ms(), std::nullopt);
        REQUIRE(std::holds_alternative<mem::SessionId>(r));
        return std::get<mem::SessionId>(r);
    });
}

template <class Pred>
bool wait_for(Pred p, std::chrono::milliseconds budget = std::chrono::milliseconds{1500}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

} // namespace

TEST_CASE("turn_writer: persists user turn on FinalTranscript") {
    auto path = tmp_db("user-turn");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);

    ev::EventBus bus;
    dlg::TurnWriter writer(bus, *mt);
    writer.set_session(sid);
    writer.start();

    bus.publish(ev::FinalTranscript{
        .turn = 1, .text = "hello there", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });

    REQUIRE(wait_for([&]{
        return mt->read([sid](mem::Repository& r) {
            auto rt = r.recent_turns(sid, 10);
            return std::holds_alternative<std::vector<mem::TurnRow>>(rt)
                && !std::get<std::vector<mem::TurnRow>>(rt).empty();
        });
    }));

    auto rows = std::get<std::vector<mem::TurnRow>>(
        mt->read([sid](mem::Repository& r){ return r.recent_turns(sid, 10); }));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == mem::TurnRole::User);
    CHECK(rows[0].status == mem::TurnStatus::Committed);
    REQUIRE(rows[0].text.has_value());
    CHECK(*rows[0].text == "hello there");
    REQUIRE(rows[0].lang.has_value());
    CHECK(*rows[0].lang == "en");

    writer.stop();
    bus.shutdown();
}

TEST_CASE("turn_writer: persists assistant turn as Committed on clean LlmFinished") {
    auto path = tmp_db("assistant-committed");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);

    ev::EventBus bus;
    dlg::TurnWriter writer(bus, *mt);
    writer.set_session(sid);
    writer.start();

    const ev::TurnId turn = 42;
    bus.publish(ev::LlmStarted{ .turn = turn });
    bus.publish(ev::LlmSentence{ .turn = turn, .seq = 0, .text = "Apples are good.", .lang = "en" });
    bus.publish(ev::LlmSentence{ .turn = turn, .seq = 1, .text = "Oranges too.",      .lang = "en" });
    bus.publish(ev::LlmFinished{ .turn = turn, .cancelled = false, .tokens_generated = 6 });

    REQUIRE(wait_for([&]{
        return mt->read([sid](mem::Repository& r) {
            auto rt = r.recent_turns(sid, 10);
            return std::holds_alternative<std::vector<mem::TurnRow>>(rt)
                && std::get<std::vector<mem::TurnRow>>(rt).size() == 1;
        });
    }));

    auto rows = std::get<std::vector<mem::TurnRow>>(
        mt->read([sid](mem::Repository& r){ return r.recent_turns(sid, 10); }));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == mem::TurnRole::Assistant);
    CHECK(rows[0].status == mem::TurnStatus::Committed);
    REQUIRE(rows[0].text.has_value());
    CHECK(*rows[0].text == "Apples are good. Oranges too.");
    CHECK(rows[0].lang.value_or("") == "en");

    writer.stop();
    bus.shutdown();
}

TEST_CASE("turn_writer: cancellation with sentences emitted writes Interrupted") {
    auto path = tmp_db("assistant-interrupted");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);

    ev::EventBus bus;
    dlg::TurnWriter writer(bus, *mt);
    writer.set_session(sid);
    writer.start();

    const ev::TurnId turn = 7;
    bus.publish(ev::LlmStarted{ .turn = turn });
    bus.publish(ev::LlmSentence{ .turn = turn, .seq = 0, .text = "Half a thought.", .lang = "en" });
    bus.publish(ev::LlmFinished{ .turn = turn, .cancelled = true, .tokens_generated = 3 });

    REQUIRE(wait_for([&]{
        return mt->read([sid](mem::Repository& r) {
            auto rt = r.recent_turns(sid, 10);
            return std::holds_alternative<std::vector<mem::TurnRow>>(rt)
                && std::get<std::vector<mem::TurnRow>>(rt).size() == 1;
        });
    }));

    auto rows = std::get<std::vector<mem::TurnRow>>(
        mt->read([sid](mem::Repository& r){ return r.recent_turns(sid, 10); }));
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == mem::TurnRole::Assistant);
    CHECK(rows[0].status == mem::TurnStatus::Interrupted);
    REQUIRE(rows[0].text.has_value());
    CHECK(*rows[0].text == "Half a thought.");
    REQUIRE(rows[0].interrupted_at_sentence.has_value());
    CHECK(*rows[0].interrupted_at_sentence == 1);

    writer.stop();
    bus.shutdown();
}

TEST_CASE("turn_writer: cancellation with NO sentences writes nothing (Discarded)") {
    auto path = tmp_db("assistant-discarded");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);

    ev::EventBus bus;
    dlg::TurnWriter writer(bus, *mt);
    writer.set_session(sid);
    writer.start();

    const ev::TurnId turn = 9;
    bus.publish(ev::LlmStarted{ .turn = turn });
    bus.publish(ev::LlmFinished{ .turn = turn, .cancelled = true, .tokens_generated = 0 });

    // Allow time for any (non-)write to be visible.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto rows = std::get<std::vector<mem::TurnRow>>(
        mt->read([sid](mem::Repository& r){ return r.recent_turns(sid, 10); }));
    CHECK(rows.empty());

    writer.stop();
    bus.shutdown();
}

TEST_CASE("turn_writer: skips writes when session not bound") {
    auto path = tmp_db("no-session");
    auto mt = open_mt(path);

    ev::EventBus bus;
    dlg::TurnWriter writer(bus, *mt);
    // session left at 0
    writer.start();

    bus.publish(ev::FinalTranscript{
        .turn = 1, .text = "ignored", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    bus.publish(ev::LlmStarted{ .turn = 1 });
    bus.publish(ev::LlmSentence{ .turn = 1, .seq = 0, .text = "x", .lang = "en" });
    bus.publish(ev::LlmFinished{ .turn = 1, .cancelled = false, .tokens_generated = 1 });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // No session ever inserted, so no rows can exist.
    auto sessions = std::get<std::vector<mem::SessionRow>>(
        mt->read([](mem::Repository& r){ return r.sessions_open_no_ended_at(); }));
    CHECK(sessions.empty());

    writer.stop();
    bus.shutdown();
}
