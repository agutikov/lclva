#include "config/config.hpp"
#include "dialogue/manager.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "llm/client.hpp"
#include "llm/prompt_builder.hpp"
#include "memory/memory_thread.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cfg = lclva::config;
namespace dlg = lclva::dialogue;
namespace ev  = lclva::event;
namespace llm = lclva::llm;
namespace mem = lclva::memory;
namespace fs  = std::filesystem;

namespace {

// Same fake LLM server style as test_llm_client. Kept inline to avoid
// shared test fixtures.
class FakeLlmServer {
public:
    struct Chunk {
        std::string body;
        std::chrono::milliseconds delay{0};
    };
    FakeLlmServer() {
        srv_.Post("/v1/chat/completions",
            [this](const httplib::Request& req, httplib::Response& res) {
                last_body_ = req.body;
                res.status = 200;
                res.set_chunked_content_provider("text/event-stream",
                    [this](std::size_t, httplib::DataSink& sink) {
                        for (const auto& c : chunks_) {
                            if (c.delay.count() > 0) std::this_thread::sleep_for(c.delay);
                            if (!sink.write(c.body.data(), c.body.size())) return false;
                        }
                        sink.done();
                        return true;
                    });
            });
        srv_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(R"({"status":"ok"})", "application/json");
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
    void set_chunks(std::vector<Chunk> c) { chunks_ = std::move(c); }
    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] std::string last_body() const { return last_body_; }
private:
    httplib::Server srv_;
    std::thread thread_;
    int port_ = 0;
    std::vector<Chunk> chunks_;
    std::string last_body_;
};

std::string sse_token(std::string_view content) {
    std::string out = R"({"choices":[{"delta":{"content":")";
    out.append(content);
    out += R"("}}]})";
    return std::string{"data: "} + out + "\n\n";
}
std::string sse_done() { return "data: [DONE]\n\n"; }

cfg::Config make_config(const std::string& base_url) {
    cfg::Config c;
    c.llm.base_url = base_url;
    c.llm.model = "fake-model";
    c.llm.temperature = 0.7;
    c.llm.max_tokens = 50;
    c.llm.request_timeout_seconds = 5;
    c.dialogue.fallback_language = "en";
    c.dialogue.system_prompts["en"] = "Test system prompt.";
    c.dialogue.recent_turns_n = 3;
    return c;
}

// Subscribes to all LlmSentence events on the bus and collects them.
class SentenceCollector {
public:
    explicit SentenceCollector(ev::EventBus& bus) {
        sub_ = bus.subscribe<ev::LlmSentence>({}, [this](const ev::LlmSentence& e) {
            std::lock_guard lk(m_);
            seen_.push_back(e);
        });
    }
    ~SentenceCollector() { if (sub_) sub_->stop(); }
    [[nodiscard]] std::vector<ev::LlmSentence> snapshot() const {
        std::lock_guard lk(m_);
        return seen_;
    }
private:
    mutable std::mutex m_;
    std::vector<ev::LlmSentence> seen_;
    ev::SubscriptionHandle sub_;
};

class FinishedCollector {
public:
    explicit FinishedCollector(ev::EventBus& bus) {
        sub_ = bus.subscribe<ev::LlmFinished>({}, [this](const ev::LlmFinished& e) {
            std::lock_guard lk(m_);
            seen_.push_back(e);
        });
    }
    ~FinishedCollector() { if (sub_) sub_->stop(); }
    [[nodiscard]] std::vector<ev::LlmFinished> snapshot() const {
        std::lock_guard lk(m_);
        return seen_;
    }
private:
    mutable std::mutex m_;
    std::vector<ev::LlmFinished> seen_;
    ev::SubscriptionHandle sub_;
};

template <class Pred>
bool wait_for(Pred p, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

std::unique_ptr<mem::MemoryThread> open_mt() {
    auto path = fs::temp_directory_path() / "lclva-mgr-test.db";
    fs::remove(path);
    auto r = mem::MemoryThread::open(path, 16);
    REQUIRE(std::holds_alternative<std::unique_ptr<mem::MemoryThread>>(r));
    return std::move(std::get<std::unique_ptr<mem::MemoryThread>>(r));
}

} // namespace

TEST_CASE("manager: FinalTranscript drives LLM and emits LlmSentence per sentence") {
    FakeLlmServer srv;
    srv.set_chunks({
        {sse_token("Hello"),       {}},
        {sse_token(", "),           {}},
        {sse_token("world"),        {}},
        {sse_token("."),            {}},
        {sse_token(" "),            {}},
        {sse_token("How are you?"), {}},
        {sse_done(),                {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    SentenceCollector  sentences(bus);
    FinishedCollector  finished(bus);

    mgr.start();

    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "hi there", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });

    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));

    auto snap = sentences.snapshot();
    REQUIRE(snap.size() >= 2);
    CHECK(snap[0].text == "Hello, world.");
    CHECK(snap[1].text == "How are you?");

    auto fin = finished.snapshot();
    REQUIRE(fin.size() == 1);
    CHECK_FALSE(fin[0].cancelled);
    // tokens_generated should equal the number of content deltas (6).
    CHECK(fin[0].tokens_generated == 6);

    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: tail without trailing punctuation flushed at end of stream") {
    FakeLlmServer srv;
    srv.set_chunks({
        {sse_token("Hello world"), {}},
        {sse_done(),                {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    SentenceCollector  sentences(bus);
    FinishedCollector  finished(bus);

    mgr.start();
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "ping", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });

    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));
    auto snap = sentences.snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].text == "Hello world");

    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: CancelGeneration aborts in-flight LLM run") {
    FakeLlmServer srv;
    // Many tokens — manager should be aborted before it consumes them all.
    std::vector<FakeLlmServer::Chunk> chunks;
    chunks.reserve(202);
    for (int i = 0; i < 200; ++i) chunks.push_back({sse_token("x"), {}});
    chunks.push_back({sse_done(), {}});
    srv.set_chunks(std::move(chunks));

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    SentenceCollector  sentences(bus);
    FinishedCollector  finished(bus);

    // Subscribe to LlmStarted to grab the manager's minted turn id, then
    // immediately publish CancelGeneration for that id.
    std::atomic<ev::TurnId> seen_turn{ev::kNoTurn};
    auto h = bus.subscribe<ev::LlmStarted>({}, [&](const ev::LlmStarted& e) {
        if (seen_turn.load() == ev::kNoTurn) {
            seen_turn.store(e.turn);
            bus.publish(ev::CancelGeneration{ .turn = e.turn });
        }
    });

    mgr.start();
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "go", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });

    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));

    auto fin = finished.snapshot();
    REQUIRE(fin.size() == 1);
    CHECK(fin[0].cancelled);
    CHECK(fin[0].tokens_generated < 200);

    h->stop();
    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: language flows from FinalTranscript to LlmSentence") {
    FakeLlmServer srv;
    srv.set_chunks({
        {sse_token("Hola, mundo. "), {}},
        {sse_done(),                  {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    cfg.dialogue.system_prompts["es"] = "Eres un asistente útil.";
    auto mt = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    SentenceCollector  sentences(bus);
    FinishedCollector  finished(bus);

    mgr.start();
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "hola", .lang = "es", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));

    auto snap = sentences.snapshot();
    REQUIRE_FALSE(snap.empty());
    CHECK(snap[0].lang == "es");

    // Spanish system prompt should have been used in the request body.
    CHECK(srv.last_body().find("Eres un asistente") != std::string::npos);

    mgr.stop();
    bus.shutdown();
}
