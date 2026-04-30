#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"
#include "memory/memory_thread.hpp"
#include "memory/repository.hpp"
#include "memory/summarizer.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace cfg = acva::config;
namespace ev  = acva::event;
namespace llm = acva::llm;
namespace mem = acva::memory;
namespace fs  = std::filesystem;

namespace {

class FakeLlmServer {
public:
    FakeLlmServer() {
        srv_.Post("/v1/chat/completions",
            [this](const httplib::Request& req, httplib::Response& res) {
                ++calls_;
                last_body_ = req.body;
                res.status = 200;
                res.set_chunked_content_provider("text/event-stream",
                    [this](std::size_t, httplib::DataSink& sink) {
                        for (const auto& c : chunks_) {
                            if (!sink.write(c.data(), c.size())) return false;
                        }
                        sink.done();
                        return true;
                    });
            });
        srv_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content("{}", "application/json");
        });
        port_ = srv_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
        for (int i = 0; i < 200 && !srv_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~FakeLlmServer() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }
    void set_chunks(std::vector<std::string> c) { chunks_ = std::move(c); }
    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] std::string last_body() const { return last_body_; }
    [[nodiscard]] int calls() const noexcept { return calls_.load(); }

private:
    httplib::Server srv_;
    std::thread thread_;
    int port_ = 0;
    std::vector<std::string> chunks_;
    std::atomic<int> calls_{0};
    std::string last_body_;
};

std::string sse(std::string_view content) {
    std::string out = R"(data: {"choices":[{"delta":{"content":")";
    out.append(content);
    out += R"("}}]})";
    out += "\n\n";
    return out;
}
std::string sse_done() { return "data: [DONE]\n\n"; }

cfg::Config make_config(const std::string& base_url, std::uint32_t turn_threshold = 4) {
    cfg::Config c;
    c.llm.base_url = base_url;
    c.llm.model = "fake";
    c.llm.temperature = 0.3;
    c.llm.max_tokens = 200;
    c.memory.summary.turn_threshold = turn_threshold;
    c.memory.summary.trigger = "turns";
    return c;
}

fs::path tmp_db(const char* name) {
    auto p = fs::temp_directory_path() / (std::string("acva-sum-") + name + ".db");
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
    return mt.read([](mem::Repository& r) {
        return std::get<mem::SessionId>(r.insert_session(mem::now_ms(), std::nullopt));
    });
}

mem::TurnId seed_turn(mem::MemoryThread& mt, mem::SessionId sid,
                      mem::TurnRole role, std::string text) {
    return mt.read([&](mem::Repository& r) {
        auto t = std::get<mem::TurnId>(r.insert_turn(sid, role, text,
            std::string("en"), mem::now_ms(), mem::TurnStatus::Committed));
        return t;
    });
}

template <class Pred>
bool wait_for(Pred p, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

} // namespace

TEST_CASE("summarizer: trigger writes a summary row with hash") {
    FakeLlmServer srv;
    srv.set_chunks({
        sse("Quick "), sse("conversation summary."),
        sse_done(),
    });

    auto path = tmp_db("trigger");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);
    seed_turn(*mt, sid, mem::TurnRole::User,      "hello");
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "hi there");

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url(), /*turn_threshold=*/2);
    llm::LlmClient client(cfg, bus);
    mem::Summarizer sum(cfg, bus, *mt, client);
    sum.set_session(sid);
    sum.start();

    sum.trigger_now();
    REQUIRE(wait_for([&]{ return sum.summaries_written() == 1; }));

    auto summaries = std::get<std::vector<mem::SummaryRow>>(
        mt->read([](mem::Repository& r){ return r.all_summaries(); }));
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].session_id == sid);
    CHECK(summaries[0].summary == "Quick conversation summary.");
    CHECK_FALSE(summaries[0].source_hash.empty());
    CHECK(summaries[0].range_start_turn >= 1);
    CHECK(summaries[0].range_end_turn   >= summaries[0].range_start_turn);

    sum.stop();
    bus.shutdown();
}

TEST_CASE("summarizer: turn_threshold drives autonomous trigger") {
    FakeLlmServer srv;
    srv.set_chunks({ sse("auto."), sse_done() });

    auto path = tmp_db("auto-trigger");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);
    // Pre-seed turns so the summarizer has something to summarize.
    for (int i = 0; i < 6; ++i) {
        seed_turn(*mt, sid, (i % 2 == 0) ? mem::TurnRole::User : mem::TurnRole::Assistant,
                  std::string("turn ") + std::to_string(i));
    }

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url(), /*turn_threshold=*/4);
    llm::LlmClient client(cfg, bus);
    mem::Summarizer sum(cfg, bus, *mt, client);
    sum.set_session(sid);
    sum.start();

    // Each LlmFinished bumps the counter by 2. Need >=4 to trigger.
    bus.publish(ev::LlmFinished{ .turn = 1, .cancelled = false, .tokens_generated = 5 });
    bus.publish(ev::LlmFinished{ .turn = 2, .cancelled = false, .tokens_generated = 5 });

    REQUIRE(wait_for([&]{ return sum.summaries_written() == 1; }));
    CHECK(srv.calls() == 1);

    sum.stop();
    bus.shutdown();
}

TEST_CASE("summarizer: cancelled LlmFinished does not advance counter") {
    FakeLlmServer srv;
    srv.set_chunks({ sse("never"), sse_done() });

    auto path = tmp_db("cancelled");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);
    seed_turn(*mt, sid, mem::TurnRole::User, "x");

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url(), /*turn_threshold=*/4);
    llm::LlmClient client(cfg, bus);
    mem::Summarizer sum(cfg, bus, *mt, client);
    sum.set_session(sid);
    sum.start();

    // 3 cancelled finishes — even at 2-per-event, must not trigger.
    for (int i = 0; i < 3; ++i) {
        bus.publish(ev::LlmFinished{ .turn = static_cast<ev::TurnId>(i + 1), .cancelled = true });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(sum.summaries_written() == 0);
    CHECK(srv.calls() == 0);

    sum.stop();
    bus.shutdown();
}

TEST_CASE("summarizer: no session bound → no work performed") {
    FakeLlmServer srv;
    srv.set_chunks({ sse("oh"), sse_done() });

    auto path = tmp_db("no-session");
    auto mt = open_mt(path);
    // session left at 0
    ev::EventBus bus;
    auto cfg = make_config(srv.base_url(), /*turn_threshold=*/2);
    llm::LlmClient client(cfg, bus);
    mem::Summarizer sum(cfg, bus, *mt, client);
    sum.start();

    sum.trigger_now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(sum.summaries_written() == 0);
    CHECK(srv.calls() == 0);

    sum.stop();
    bus.shutdown();
}

TEST_CASE("summarizer: subsequent run covers turns added since last summary") {
    FakeLlmServer srv;
    srv.set_chunks({ sse("first summary."), sse_done() });

    auto path = tmp_db("incremental");
    auto mt = open_mt(path);
    auto sid = seed_session(*mt);
    seed_turn(*mt, sid, mem::TurnRole::User,      "u1");
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "a1");

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url(), /*turn_threshold=*/2);
    llm::LlmClient client(cfg, bus);
    mem::Summarizer sum(cfg, bus, *mt, client);
    sum.set_session(sid);
    sum.start();
    sum.trigger_now();
    REQUIRE(wait_for([&]{ return sum.summaries_written() == 1; }));

    // Add more turns; the next trigger should cover only the new ones.
    seed_turn(*mt, sid, mem::TurnRole::User,      "u2");
    seed_turn(*mt, sid, mem::TurnRole::Assistant, "a2");
    sum.trigger_now();
    REQUIRE(wait_for([&]{ return sum.summaries_written() == 2; }));

    auto sums = std::get<std::vector<mem::SummaryRow>>(
        mt->read([](mem::Repository& r){ return r.all_summaries(); }));
    REQUIRE(sums.size() == 2);
    CHECK(sums[1].range_start_turn > sums[0].range_end_turn);

    sum.stop();
    bus.shutdown();
}
