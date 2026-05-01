#include "supervisor/probe.hpp"

#include <httplib.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using acva::supervisor::HttpProbe;
using acva::supervisor::parse_url;
using acva::supervisor::ProbeResult;

namespace {

// Tiny HTTP fixture: a httplib::Server bound to a localhost ephemeral
// port on a worker thread. Tests register handlers before start(), and
// stop() drains the listen loop on teardown. Bound port is read back
// after the listener thread has called bind_to_any_port().
class TestServer {
public:
    TestServer() {
        port_ = server_.bind_to_any_port("127.0.0.1");
        REQUIRE(port_ > 0);
    }

    ~TestServer() {
        stop();
    }

    httplib::Server& server() noexcept { return server_; }
    int port() const noexcept { return port_; }

    // Block until the server reports it is listening, then return.
    void start() {
        thread_ = std::thread([this] {
            (void)server_.listen_after_bind();
        });
        // Cheap spin: cpp-httplib doesn't expose a "ready" handle, but
        // bind_to_any_port has already locked the port so 5 ms is plenty.
        for (int i = 0; i < 50 && !server_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void stop() {
        if (thread_.joinable()) {
            server_.stop();
            thread_.join();
        }
    }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

private:
    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;
};

} // namespace

TEST_CASE("probe::parse_url splits scheme, authority, path") {
    {
        auto p = parse_url("http://127.0.0.1:8081/health");
        CHECK(p.authority == "http://127.0.0.1:8081");
        CHECK(p.path == "/health");
    }
    {
        auto p = parse_url("https://example.com");
        CHECK(p.authority == "https://example.com");
        CHECK(p.path == "/");
    }
    {
        // No scheme — refused.
        auto p = parse_url("127.0.0.1:8081/health");
        CHECK(p.authority.empty());
        CHECK(p.path.empty());
    }
    {
        // Path with query.
        auto p = parse_url("http://h/health?ok=1");
        CHECK(p.authority == "http://h");
        CHECK(p.path == "/health?ok=1");
    }
}

TEST_CASE("probe: 200 OK is reported as ok=true") {
    TestServer ts;
    ts.server().Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });
    ts.start();

    HttpProbe probe(std::chrono::milliseconds(500));
    auto r = probe.get(ts.url("/health"));
    CHECK(r.ok);
    CHECK(r.http_status == 200);
    CHECK(r.body_excerpt.empty());
    CHECK(r.latency >= std::chrono::milliseconds(0));
}

TEST_CASE("probe: 5xx is reported as ok=false with body excerpt") {
    TestServer ts;
    ts.server().Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 500;
        res.set_content("model not loaded yet", "text/plain");
    });
    ts.start();

    HttpProbe probe(std::chrono::milliseconds(500));
    auto r = probe.get(ts.url("/health"));
    CHECK_FALSE(r.ok);
    CHECK(r.http_status == 500);
    CHECK(r.body_excerpt.find("model not loaded") != std::string::npos);
}

TEST_CASE("probe: connection refused returns ok=false, http_status=0") {
    HttpProbe probe(std::chrono::milliseconds(200));
    // Port 1 is privileged on Linux and effectively never bound by tests.
    auto r = probe.get("http://127.0.0.1:1/health");
    CHECK_FALSE(r.ok);
    CHECK(r.http_status == 0);
    CHECK_FALSE(r.body_excerpt.empty());
}

TEST_CASE("probe: timeout returns ok=false, http_status=0") {
    TestServer ts;
    ts.server().Get("/slow", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        res.set_content("late", "text/plain");
    });
    ts.start();

    HttpProbe probe(std::chrono::milliseconds(50));
    auto r = probe.get(ts.url("/slow"));
    CHECK_FALSE(r.ok);
    CHECK(r.http_status == 0);
    CHECK_FALSE(r.body_excerpt.empty());
    // The probe must not block much longer than the timeout. Generous
    // upper bound: 4× the configured timeout. Tightens to flaky on
    // overloaded hosts otherwise.
    CHECK(r.latency < std::chrono::milliseconds(800));
}

TEST_CASE("probe: invalid url (no scheme) is rejected without network call") {
    HttpProbe probe(std::chrono::milliseconds(500));
    auto r = probe.get("127.0.0.1:8081/health");
    CHECK_FALSE(r.ok);
    CHECK(r.http_status == 0);
    CHECK(r.body_excerpt.find("invalid url") != std::string::npos);
    // Must have returned essentially instantly.
    CHECK(r.latency < std::chrono::milliseconds(50));
}

TEST_CASE("probe: 2xx (not just 200) counts as ok") {
    TestServer ts;
    ts.server().Get("/created", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    ts.start();

    HttpProbe probe(std::chrono::milliseconds(500));
    auto r = probe.get(ts.url("/created"));
    CHECK(r.ok);
    CHECK(r.http_status == 204);
}
