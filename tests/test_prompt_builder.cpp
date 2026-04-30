#include "config/config.hpp"
#include "llm/prompt_builder.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <variant>

namespace cfg = lclva::config;
namespace llm = lclva::llm;
namespace mem = lclva::memory;
namespace fs  = std::filesystem;

namespace {

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("lclva-prompt-") + name + ".db");
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p;
}

cfg::Config make_config() {
    cfg::Config c;
    c.llm.model            = "qwen2.5-7b-instruct";
    c.llm.temperature      = 0.7;
    c.llm.max_tokens       = 400;
    c.dialogue.recent_turns_n     = 3;
    c.dialogue.fallback_language  = "en";
    c.dialogue.system_prompts["en"] = "You are a helpful assistant.";
    c.memory.facts.confidence_threshold = 0.5;
    return c;
}

std::unique_ptr<mem::MemoryThread> open_mt(const fs::path& p) {
    auto r = mem::MemoryThread::open(p, /*queue_capacity=*/16);
    REQUIRE(std::holds_alternative<std::unique_ptr<mem::MemoryThread>>(r));
    return std::move(std::get<std::unique_ptr<mem::MemoryThread>>(r));
}

mem::SessionId seed_session(mem::MemoryThread& mt) {
    return mt.read([](mem::Repository& repo) {
        auto sid_r = repo.insert_session(/*started_at=*/1'700'000'000'000, std::nullopt);
        REQUIRE(std::holds_alternative<mem::SessionId>(sid_r));
        return std::get<mem::SessionId>(sid_r);
    });
}

mem::TurnId seed_turn(mem::MemoryThread& mt, mem::SessionId sid,
                      mem::TurnRole role, std::string text,
                      mem::TurnStatus status = mem::TurnStatus::Committed) {
    return mt.read([&](mem::Repository& repo) {
        auto t_r = repo.insert_turn(sid, role, text, std::string("en"),
                                    mem::now_ms(), status);
        REQUIRE(std::holds_alternative<mem::TurnId>(t_r));
        return std::get<mem::TurnId>(t_r);
    });
}

} // namespace

TEST_CASE("prompt_builder: minimal — no session, no memory") {
    auto p = tmp_db("minimal");
    auto mt = open_mt(p);
    auto config = make_config();
    llm::PromptBuilder b(config, *mt);

    auto body = b.build({.session_id = 0, .lang = "en", .current_user_text = "hi"});

    CHECK(body.find(R"("model":"qwen2.5-7b-instruct")")        != std::string::npos);
    CHECK(body.find(R"("role":"system")")                        != std::string::npos);
    CHECK(body.find("You are a helpful assistant.")              != std::string::npos);
    CHECK(body.find(R"("role":"user","content":"hi")")           != std::string::npos);
    CHECK(body.find(R"("temperature":0.7)")                       != std::string::npos);
    CHECK(body.find(R"("max_tokens":400)")                        != std::string::npos);
    CHECK(body.find(R"("stream":true)")                           != std::string::npos);

    // Token estimate is 1/4 of byte length, rounded up.
    CHECK(b.last_token_estimate() == (body.size() + 3) / 4);
}

TEST_CASE("prompt_builder: includes recent committed turns oldest-first") {
    auto p = tmp_db("turns");
    auto mt = open_mt(p);
    auto config = make_config();
    llm::PromptBuilder b(config, *mt);

    auto sid = seed_session(*mt);
    auto t1 = seed_turn(*mt, sid, mem::TurnRole::User,      "first user");
    auto t2 = seed_turn(*mt, sid, mem::TurnRole::Assistant, "first assistant reply");
    (void)t1; (void)t2;

    auto body = b.build({.session_id = sid, .lang = "en", .current_user_text = "follow up"});

    // Order of historical turns (system → t1 → t2 → current user).
    auto p_sys = body.find(R"("role":"system")");
    auto p_u1  = body.find(R"("role":"user","content":"first user")");
    auto p_a1  = body.find(R"("role":"assistant","content":"first assistant reply")");
    auto p_cur = body.find(R"("role":"user","content":"follow up")");
    REQUIRE(p_sys != std::string::npos);
    REQUIRE(p_u1  != std::string::npos);
    REQUIRE(p_a1  != std::string::npos);
    REQUIRE(p_cur != std::string::npos);
    CHECK(p_sys < p_u1);
    CHECK(p_u1  < p_a1);
    CHECK(p_a1  < p_cur);
}

TEST_CASE("prompt_builder: drops in_progress and discarded turns") {
    auto p = tmp_db("filter-status");
    auto mt = open_mt(p);
    auto config = make_config();
    config.dialogue.recent_turns_n = 10; // > number of seeded rows so limit doesn't mask filter
    llm::PromptBuilder b(config, *mt);

    auto sid = seed_session(*mt);
    seed_turn(*mt, sid, mem::TurnRole::User,      "kept committed", mem::TurnStatus::Committed);
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "kept interrupted partial",
              mem::TurnStatus::Interrupted);
    seed_turn(*mt, sid, mem::TurnRole::User,      "should not appear",
              mem::TurnStatus::InProgress);
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "also gone", mem::TurnStatus::Discarded);

    auto body = b.build({.session_id = sid, .lang = "en", .current_user_text = "next"});

    CHECK(body.find("kept committed")              != std::string::npos);
    CHECK(body.find("kept interrupted partial")    != std::string::npos);
    CHECK(body.find("should not appear")           == std::string::npos);
    CHECK(body.find("also gone")                   == std::string::npos);
}

TEST_CASE("prompt_builder: facts above confidence threshold appear in system message") {
    auto p = tmp_db("facts");
    auto mt = open_mt(p);
    auto config = make_config(); // threshold = 0.5
    llm::PromptBuilder b(config, *mt);

    auto sid = seed_session(*mt);
    mt->read([&](mem::Repository& repo) {
        (void)repo.upsert_fact("name",     std::nullopt, "Aleksei",
                               std::nullopt, /*conf=*/0.95, mem::now_ms());
        (void)repo.upsert_fact("uncertain", std::nullopt, "guess",
                               std::nullopt, /*conf=*/0.30, mem::now_ms());
        return 0;
    });

    auto body = b.build({.session_id = sid, .lang = "en", .current_user_text = "hi"});

    CHECK(body.find("name: Aleksei") != std::string::npos);
    CHECK(body.find("uncertain")     == std::string::npos);
    CHECK(body.find("Durable user facts:") != std::string::npos);
}

TEST_CASE("prompt_builder: latest summary embedded in system message") {
    auto p = tmp_db("summary");
    auto mt = open_mt(p);
    auto config = make_config();
    llm::PromptBuilder b(config, *mt);

    auto sid = seed_session(*mt);
    mt->read([&](mem::Repository& repo) {
        (void)repo.insert_summary(sid, /*range_start=*/1, /*range_end=*/5,
                                  "User asked about Greek philosophy.",
                                  "en", "hash-1", mem::now_ms());
        (void)repo.insert_summary(sid, /*range_start=*/6, /*range_end=*/10,
                                  "User pivoted to FFmpeg pipeline tuning.",
                                  "en", "hash-2", mem::now_ms());
        return 0;
    });

    auto body = b.build({.session_id = sid, .lang = "en", .current_user_text = "ok"});

    // latest_summary picks the highest range_end_turn → "FFmpeg".
    CHECK(body.find("Earlier conversation summary:") != std::string::npos);
    CHECK(body.find("User pivoted to FFmpeg pipeline tuning.") != std::string::npos);
    CHECK(body.find("Greek philosophy") == std::string::npos);
}

TEST_CASE("prompt_builder: lang fallback when prompt missing for requested language") {
    auto p = tmp_db("fallback");
    auto mt = open_mt(p);
    auto config = make_config();
    config.dialogue.system_prompts["fr"] = "Tu es un assistant utile.";
    // No "ru" prompt — should fall back to "en".
    llm::PromptBuilder b(config, *mt);

    auto body_fr = b.build({.session_id = 0, .lang = "fr", .current_user_text = "salut"});
    CHECK(body_fr.find("Tu es un assistant utile.") != std::string::npos);

    auto body_ru = b.build({.session_id = 0, .lang = "ru", .current_user_text = "privet"});
    CHECK(body_ru.find("You are a helpful assistant.") != std::string::npos);
}

TEST_CASE("prompt_builder: snapshot — stable JSON for fixed inputs") {
    auto p = tmp_db("snapshot");
    auto mt = open_mt(p);
    auto config = make_config();
    config.dialogue.recent_turns_n = 1;
    llm::PromptBuilder b(config, *mt);

    auto sid = seed_session(*mt);
    seed_turn(*mt, sid, mem::TurnRole::User,      "earlier");
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "earlier-reply");

    auto a = b.build({.session_id = sid, .lang = "en", .current_user_text = "now"});
    auto bb = b.build({.session_id = sid, .lang = "en", .current_user_text = "now"});
    CHECK(a == bb); // byte-identical across calls
}
