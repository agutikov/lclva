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

namespace cfg = acva::config;
namespace dlg = acva::dialogue;
namespace ev  = acva::event;
namespace llm = acva::llm;
namespace mem = acva::memory;
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
    auto path = fs::temp_directory_path() / "acva-mgr-test.db";
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

// --- M2.5: pipeline gating ---

TEST_CASE("manager: closed gate refuses turn, no LlmFinished, UserInterrupted published") {
    FakeLlmServer srv;
    // The server should NOT be hit. Stage chunks anyway so that a
    // regression (manager submits despite the closed gate) shows up as
    // a clear "saw sentences when we shouldn't have".
    srv.set_chunks({
        {sse_token("should not run"), {}},
        {sse_done(),                   {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    // Capture UserInterrupted as a signal that gating fired.
    std::atomic<int> interrupts{0};
    auto h = bus.subscribe<ev::UserInterrupted>({}, [&](const ev::UserInterrupted&) {
        interrupts.fetch_add(1);
    });

    SentenceCollector  sentences(bus);
    FinishedCollector  finished(bus);

    mgr.set_pipeline_gate([]{ return false; });   // gate closed
    mgr.start();

    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "ignored", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });

    // Wait for the gate to take effect: gated_turns increments and
    // UserInterrupted gets dispatched. Both happen on the subscriber
    // thread synchronously with the FinalTranscript dispatch.
    REQUIRE(wait_for([&]{ return mgr.gated_turns() >= 1; }));
    REQUIRE(wait_for([&]{ return interrupts.load() >= 1; }));

    // Give any phantom LLM work time to surface — none should.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(finished.snapshot().empty());
    CHECK(sentences.snapshot().empty());

    h->stop();
    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: gate flip from closed to open lets the next turn through") {
    FakeLlmServer srv;
    srv.set_chunks({
        {sse_token("Hello. "), {}},
        {sse_done(),            {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    std::atomic<bool> open{false};
    SentenceCollector sentences(bus);
    FinishedCollector finished(bus);

    mgr.set_pipeline_gate([&]{ return open.load(); });
    mgr.start();

    // First turn is gated.
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "first", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    REQUIRE(wait_for([&]{ return mgr.gated_turns() >= 1; }));
    CHECK(finished.snapshot().empty());

    // Open the gate; the next turn should reach the LLM.
    open.store(true);
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "second", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));
    auto fin = finished.snapshot();
    REQUIRE(fin.size() == 1);
    CHECK_FALSE(fin[0].cancelled);
    CHECK(mgr.gated_turns() == 1);   // unchanged

    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: max_assistant_sentences caps emission and cancels the LLM") {
    FakeLlmServer srv;
    // Pack four sentence boundaries into the first delta so the
    // splitter actually emits ≥ cap sentences mid-stream (it has
    // one-sentence lookahead — `One.` only emits when it sees the
    // capital `T` of the next sentence). The trailing `Six.` gives
    // the cap-cancel path a clear signal to abort instead of running
    // to completion.
    srv.set_chunks({
        {sse_token("One. Two. Three. Four. "), {}},
        {sse_token("Five. "),                   {}},
        {sse_token("Six."),                     {}},
        {sse_done(),                             {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    cfg.dialogue.max_assistant_sentences = 3;
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    SentenceCollector sentences(bus);
    FinishedCollector finished(bus);

    mgr.start();
    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "list five things", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));

    auto snap = sentences.snapshot();
    // Exactly the cap-many LlmSentence events were published — no more,
    // even though the server kept sending and the splitter would
    // otherwise have emitted "Six." at end-of-stream. Verifying the
    // count is the cap-behavior contract; the per-sentence text is a
    // SentenceSplitter property and lives in test_sentence_splitter.
    REQUIRE(snap.size() == 3);

    // The LLM stream was cancelled to stop further generation;
    // LlmClient reports cancelled=true (libcurl's write callback
    // returned 0 once the token flipped).
    auto fin = finished.snapshot();
    REQUIRE(fin.size() == 1);
    CHECK(fin[0].cancelled);
    // Fewer than the full set of deltas should have been processed —
    // we cancelled mid-stream, so tokens_generated < 3 (the number of
    // SSE chunks the server sent).
    CHECK(fin[0].tokens_generated < 3);

    mgr.stop();
    bus.shutdown();
}

TEST_CASE("manager: throwing gate predicate is treated as open") {
    FakeLlmServer srv;
    srv.set_chunks({
        {sse_token("ok. "), {}},
        {sse_done(),         {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    auto mt  = open_mt();
    llm::PromptBuilder pb(cfg, *mt);
    llm::LlmClient client(cfg, bus);
    dlg::TurnFactory turns;
    dlg::Manager mgr(cfg, bus, pb, client, turns);

    FinishedCollector finished(bus);

    mgr.set_pipeline_gate([]() -> bool { throw std::runtime_error("boom"); });
    mgr.start();

    bus.publish(ev::FinalTranscript{
        .turn = 0, .text = "ping", .lang = "en", .confidence = 1.0F,
        .audio_duration = {}, .processing_duration = {},
    });
    REQUIRE(wait_for([&]{ return !finished.snapshot().empty(); }));
    CHECK(mgr.gated_turns() == 0);

    mgr.stop();
    bus.shutdown();
}
