#include "config/config.hpp"
#include "dialogue/turn.hpp"
#include "event/bus.hpp"
#include "llm/client.hpp"

#include <doctest/doctest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace cfg = acva::config;
namespace dlg = acva::dialogue;
namespace ev  = acva::event;
namespace llm = acva::llm;

namespace {

// Tiny SSE fake. The handler emits chunks supplied by the test,
// optionally pausing between them so cancellation tests have a
// well-defined interleave point.
class FakeLlmServer {
public:
    struct Chunk {
        std::string body;
        std::chrono::milliseconds delay{0};
    };

    FakeLlmServer() {
        srv_.Post("/v1/chat/completions",
            [this](const httplib::Request& req, httplib::Response& res) {
                ++request_count_;
                last_request_body_ = req.body;
                res.status = response_status_;
                if (response_status_ != 200) {
                    res.set_content("error", "text/plain");
                    return;
                }
                res.set_chunked_content_provider("text/event-stream",
                    [this](std::size_t /*offset*/, httplib::DataSink& sink) {
                        for (const auto& c : chunks_) {
                            if (c.delay.count() > 0) std::this_thread::sleep_for(c.delay);
                            if (!sink.write(c.body.data(), c.body.size())) return false;
                        }
                        sink.done();
                        return true;
                    });
            });
        srv_.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.status = health_status_;
            res.set_content(R"({"status":"ok"})", "application/json");
        });
        port_ = srv_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this]{ srv_.listen_after_bind(); });
        // Wait for listener readiness.
        for (int i = 0; i < 200 && !srv_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~FakeLlmServer() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void set_chunks(std::vector<Chunk> c) { chunks_ = std::move(c); }
    void set_response_status(int s)        { response_status_ = s; }
    void set_health_status(int s)          { health_status_ = s; }
    [[nodiscard]] int port() const noexcept { return port_; }
    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] int request_count() const noexcept { return request_count_; }
    [[nodiscard]] std::string last_request_body() const { return last_request_body_; }

private:
    httplib::Server srv_;
    std::thread thread_;
    int port_ = 0;
    int response_status_ = 200;
    int health_status_   = 200;
    std::vector<Chunk> chunks_;
    std::atomic<int> request_count_{0};
    std::string last_request_body_;
};

cfg::Config make_config(const std::string& base_url) {
    cfg::Config c;
    c.llm.base_url = base_url;
    c.llm.model = "fake-model";
    c.llm.temperature = 0.7;
    c.llm.max_tokens = 8;
    c.llm.request_timeout_seconds = 5;
    return c;
}

std::string sse(std::string_view payload) {
    return std::string{"data: "} + std::string{payload} + "\n\n";
}

std::string sse_done() { return "data: [DONE]\n\n"; }

std::string token_chunk(std::string_view content) {
    return sse(std::string{R"({"choices":[{"delta":{"content":")"}
                + std::string{content}
                + R"("}}]})");
}

} // namespace

TEST_CASE("llm: streams tokens from SSE response") {
    FakeLlmServer srv;
    srv.set_chunks({
        {token_chunk("Hello"), {}},
        {token_chunk(" "),     {}},
        {token_chunk("world"), {}},
        {sse_done(),           {}},
    });

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    llm::LlmClient client(cfg, bus);

    auto ctx = dlg::TurnFactory{}.mint();
    std::string captured;
    llm::LlmFinish finish{};
    bool finished_called = false;

    client.submit(llm::LlmRequest{
        .body = R"({"messages":[{"role":"user","content":"hi"}]})",
        .cancel = ctx.token,
        .turn   = ctx.id,
        .lang   = "en",
    }, llm::LlmCallbacks{
        .on_token    = [&](std::string_view t) { captured.append(t); },
        .on_finished = [&](llm::LlmFinish f)   { finish = f; finished_called = true; },
    });

    bus.shutdown();

    REQUIRE(finished_called);
    CHECK(captured == "Hello world");
    CHECK(finish.turn == ctx.id);
    CHECK_FALSE(finish.cancelled);
    CHECK_FALSE(finish.error);
    CHECK(finish.tokens_generated == 3);
}

TEST_CASE("llm: cancellation aborts within 100 ms after token-arrival cancel") {
    FakeLlmServer srv;
    // 200 chunks back-to-back — like a real LLM streaming at hundreds of
    // tokens/sec. Cancellation must abort while the stream is still in
    // flight, so the test cancels immediately on the first token.
    std::vector<FakeLlmServer::Chunk> chunks;
    chunks.reserve(202);
    for (int i = 0; i < 200; ++i) chunks.push_back({token_chunk("x"), {}});
    chunks.push_back({sse_done(), {}});
    srv.set_chunks(std::move(chunks));

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    llm::LlmClient client(cfg, bus);

    auto ctx = dlg::TurnFactory{}.mint();

    int tokens_seen = 0;
    std::chrono::steady_clock::time_point cancel_at{};
    bool cancel_fired = false;
    llm::LlmFinish finish{};

    auto on_token = [&](std::string_view) {
        ++tokens_seen;
        if (!cancel_fired) {
            cancel_at = std::chrono::steady_clock::now();
            ctx.token->cancel();
            cancel_fired = true;
        }
    };
    auto on_finished = [&](llm::LlmFinish f) { finish = f; };

    client.submit(llm::LlmRequest{
        .body   = R"({"messages":[]})",
        .cancel = ctx.token,
        .turn   = ctx.id,
        .lang   = "en",
    }, llm::LlmCallbacks{ .on_token = on_token, .on_finished = on_finished });

    const auto cancel_to_finish =
        std::chrono::steady_clock::now() - cancel_at;
    bus.shutdown();

    REQUIRE(cancel_fired);
    CHECK(cancel_to_finish < std::chrono::milliseconds{100});
    CHECK(finish.cancelled);
    CHECK_FALSE(finish.error);
    CHECK(tokens_seen < 200); // proved we aborted before draining
}

TEST_CASE("llm: probe true on /health 200, false on 503") {
    FakeLlmServer srv;
    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    llm::LlmClient client(cfg, bus);

    CHECK(client.probe());

    srv.set_health_status(503);
    CHECK_FALSE(client.probe());

    bus.shutdown();
}

TEST_CASE("llm: non-200 chat response surfaces as error, never cancelled") {
    FakeLlmServer srv;
    srv.set_response_status(500);
    srv.set_chunks({}); // unused on the error path

    ev::EventBus bus;
    auto cfg = make_config(srv.base_url());
    llm::LlmClient client(cfg, bus);

    auto ctx = dlg::TurnFactory{}.mint();
    llm::LlmFinish finish{};
    client.submit(llm::LlmRequest{
        .body   = R"({"messages":[]})",
        .cancel = ctx.token,
        .turn   = ctx.id,
        .lang   = "en",
    }, llm::LlmCallbacks{
        .on_token    = [&](std::string_view) {},
        .on_finished = [&](llm::LlmFinish f) { finish = f; },
    });

    bus.shutdown();

    CHECK(finish.error);
    CHECK_FALSE(finish.cancelled);
    CHECK(finish.tokens_generated == 0);
    CHECK(finish.error_message.find("500") != std::string::npos);
}

TEST_CASE("llm: emits LlmFinished on the bus") {
    FakeLlmServer srv;
    srv.set_chunks({{token_chunk("ok"), {}}, {sse_done(), {}}});

    ev::EventBus bus;
    std::atomic<int> finished_count{0};
    std::atomic<ev::TurnId> finished_turn{0};
    bus.subscribe<ev::LlmFinished>({}, [&](const ev::LlmFinished& e) {
        finished_turn = e.turn;
        ++finished_count;
    });

    auto cfg = make_config(srv.base_url());
    llm::LlmClient client(cfg, bus);

    auto ctx = dlg::TurnFactory{}.mint();
    client.submit(llm::LlmRequest{
        .body = R"({"messages":[]})",
        .cancel = ctx.token,
        .turn = ctx.id,
        .lang = "en",
    }, llm::LlmCallbacks{
        .on_token = [](std::string_view){},
        .on_finished = [](llm::LlmFinish){},
    });

    // Allow the bus dispatcher to deliver.
    for (int i = 0; i < 200 && finished_count.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bus.shutdown();
    CHECK(finished_count.load() == 1);
    CHECK(finished_turn.load() == ctx.id);
}
