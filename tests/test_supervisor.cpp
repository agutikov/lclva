#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"
#include "supervisor/supervisor.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

using acva::config::SupervisorConfig;
using acva::event::ErrorEvent;
using acva::event::EventBus;
using acva::supervisor::PipelineState;
using acva::supervisor::ProbeResult;
using acva::supervisor::ServiceConfig;
using acva::supervisor::ServiceState;
using acva::supervisor::Supervisor;

namespace {

// Per-service mock — shares its state across the supervisor and all
// monitors via a single ProbeFn. Each registered service gets its own
// row keyed by URL, so flipping one service to fail doesn't affect
// the others.
class MultiMockProbe {
public:
    void set_ok(const std::string& url, bool ok) {
        std::lock_guard lk(mu_);
        oks_[url] = ok;
    }

    [[nodiscard]] auto fn() {
        return [this](std::string_view url) -> ProbeResult {
            std::lock_guard lk(mu_);
            const std::string key{url};
            const bool ok = oks_.contains(key) ? oks_[key] : true;
            return ProbeResult{
                .ok           = ok,
                .http_status  = ok ? 200 : 500,
                .latency      = std::chrono::milliseconds(1),
                .body_excerpt = ok ? "" : "down",
            };
        };
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, bool> oks_;
};

ServiceConfig make_svc(const std::string& name, const std::string& url,
                       bool fail_pipeline = true) {
    ServiceConfig c;
    c.name = name;
    c.health_url = url;
    c.fail_pipeline_if_down = fail_pipeline;
    c.probe_interval_healthy  = std::chrono::milliseconds(2);
    c.probe_interval_degraded = std::chrono::milliseconds(2);
    c.degraded_max_failures   = 2;
    return c;
}

SupervisorConfig sup_cfg(std::uint32_t grace_seconds) {
    SupervisorConfig c;
    c.pipeline_fail_grace_seconds = grace_seconds;
    c.probe_timeout_ms = 100;
    return c;
}

bool wait_for_state(const Supervisor& sup, ServiceState s, const std::string& name,
                    std::chrono::milliseconds budget = std::chrono::milliseconds(800)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& svc : sup.snapshot().services) {
            if (svc.name == name && svc.state == s) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("Supervisor: no critical services → pipeline_state == NoConfiguredCriticalServices") {
    EventBus bus;
    MultiMockProbe probe;
    Supervisor sup(sup_cfg(30), bus, probe.fn());
    sup.register_service(make_svc("tts", "", /*fail*/ false));
    sup.start();
    auto s = sup.wait_for_pipeline_state(PipelineState::NoConfiguredCriticalServices,
                                          std::chrono::milliseconds(50));
    CHECK(s == PipelineState::NoConfiguredCriticalServices);
    sup.stop();
}

TEST_CASE("Supervisor: all critical services healthy → pipeline_state == Ok") {
    EventBus bus;
    MultiMockProbe probe;
    probe.set_ok("http://127.0.0.1:1/llm", true);

    Supervisor sup(sup_cfg(30), bus, probe.fn());
    sup.register_service(make_svc("llm", "http://127.0.0.1:1/llm"));
    sup.start();

    REQUIRE(wait_for_state(sup, ServiceState::Healthy, "llm"));
    auto s = sup.wait_for_pipeline_state(PipelineState::Ok, std::chrono::milliseconds(200));
    CHECK(s == PipelineState::Ok);
    sup.stop();
}

TEST_CASE("Supervisor: critical service Unhealthy past grace → Failed; ErrorEvent published") {
    EventBus bus;
    // Capture ErrorEvents.
    std::atomic<int> errors{0};
    auto err_sub = bus.subscribe<ErrorEvent>({}, [&](const ErrorEvent& e) {
        if (e.component == "supervisor") errors.fetch_add(1);
    });

    MultiMockProbe probe;
    probe.set_ok("http://127.0.0.1:1/llm", false);
    Supervisor sup(sup_cfg(0), bus, probe.fn());   // 0 s grace → trips immediately
    sup.register_service(make_svc("llm", "http://127.0.0.1:1/llm"));
    sup.start();

    auto s = sup.wait_for_pipeline_state(PipelineState::Failed,
                                          std::chrono::milliseconds(2000));
    CHECK(s == PipelineState::Failed);
    // Allow the bus dispatcher to deliver.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(errors.load() == 1);
    sup.stop();
}

TEST_CASE("Supervisor: recovery clears Failed → Ok") {
    EventBus bus;
    MultiMockProbe probe;
    probe.set_ok("http://127.0.0.1:1/llm", false);
    Supervisor sup(sup_cfg(0), bus, probe.fn());
    sup.register_service(make_svc("llm", "http://127.0.0.1:1/llm"));
    sup.start();

    REQUIRE(sup.wait_for_pipeline_state(PipelineState::Failed,
                                         std::chrono::milliseconds(2000))
            == PipelineState::Failed);

    probe.set_ok("http://127.0.0.1:1/llm", true);
    REQUIRE(wait_for_state(sup, ServiceState::Healthy, "llm",
                            std::chrono::milliseconds(500)));
    auto s = sup.wait_for_pipeline_state(PipelineState::Ok,
                                          std::chrono::milliseconds(2000));
    CHECK(s == PipelineState::Ok);
    sup.stop();
}

TEST_CASE("Supervisor: non-critical service Unhealthy does not trip the pipeline") {
    EventBus bus;
    MultiMockProbe probe;
    probe.set_ok("http://127.0.0.1:1/llm", true);
    probe.set_ok("http://127.0.0.1:1/tts", false);

    Supervisor sup(sup_cfg(0), bus, probe.fn());
    sup.register_service(make_svc("llm", "http://127.0.0.1:1/llm", /*fail*/ true));
    sup.register_service(make_svc("tts", "http://127.0.0.1:1/tts", /*fail*/ false));
    sup.start();

    REQUIRE(wait_for_state(sup, ServiceState::Healthy,   "llm"));
    REQUIRE(wait_for_state(sup, ServiceState::Unhealthy, "tts"));
    auto s = sup.wait_for_pipeline_state(PipelineState::Ok,
                                          std::chrono::milliseconds(500));
    CHECK(s == PipelineState::Ok);
    sup.stop();
}

TEST_CASE("Supervisor: snapshot lists every registered service") {
    EventBus bus;
    MultiMockProbe probe;
    Supervisor sup(sup_cfg(30), bus, probe.fn());
    sup.register_service(make_svc("llm", "http://127.0.0.1:1/llm"));
    sup.register_service(make_svc("stt", "http://127.0.0.1:1/stt"));
    sup.register_service(make_svc("tts", "", /*fail*/ false));
    sup.start();
    auto snap = sup.snapshot();
    REQUIRE(snap.services.size() == 3);
    // Order matches registration.
    CHECK(snap.services[0].name == "llm");
    CHECK(snap.services[1].name == "stt");
    CHECK(snap.services[2].name == "tts");
    CHECK(snap.services[2].state == ServiceState::NotConfigured);
    sup.stop();
}
