#include "event/bus.hpp"
#include "event/event.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

using acva::event::EventBus;
using acva::event::HealthChanged;
using acva::event::HealthState;
using acva::supervisor::ProbeResult;
using acva::supervisor::ServiceConfig;
using acva::supervisor::ServiceMonitor;
using acva::supervisor::ServiceState;

namespace {

// Programmable probe — pops the next ProbeResult off a queue under a
// mutex and counts probe calls. When the queue is empty it returns the
// last result repeatedly so the loop has something to chew on.
class MockProbe {
public:
    void enqueue(ProbeResult r) {
        std::lock_guard lk(mu_);
        results_.push_back(std::move(r));
    }
    void enqueue_ok(int n = 1) {
        for (int i = 0; i < n; ++i) {
            enqueue(ProbeResult{
                .ok = true, .http_status = 200,
                .latency = std::chrono::milliseconds(0),
                .body_excerpt = {},
            });
        }
    }
    void enqueue_fail(int n = 1) {
        for (int i = 0; i < n; ++i) {
            enqueue(ProbeResult{
                .ok = false, .http_status = 0,
                .latency = std::chrono::milliseconds(0),
                .body_excerpt = "down",
            });
        }
    }
    [[nodiscard]] std::uint64_t calls() const noexcept { return calls_.load(); }

    [[nodiscard]] auto fn() {
        return [this](std::string_view /*url*/) -> ProbeResult {
            calls_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(mu_);
            if (!results_.empty()) {
                last_ = results_.front();
                results_.pop_front();
            }
            return last_;
        };
    }

private:
    std::mutex mu_;
    std::deque<ProbeResult> results_;
    ProbeResult last_;          // returned when queue empty
    std::atomic<std::uint64_t> calls_{0};
};

// Bus subscriber that captures HealthChanged events.
class HealthSink {
public:
    explicit HealthSink(EventBus& bus) {
        sub_ = bus.subscribe<HealthChanged>({}, [this](const HealthChanged& e) {
            std::lock_guard lk(mu_);
            events_.push_back(e);
        });
    }
    [[nodiscard]] std::vector<HealthChanged> snapshot() const {
        std::lock_guard lk(mu_);
        return events_;
    }
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(mu_);
        return events_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<HealthChanged> events_;
    acva::event::SubscriptionHandle sub_;
};

ServiceConfig make_cfg() {
    ServiceConfig c;
    c.name = "llm";
    c.health_url = "http://127.0.0.1:8081/health";
    c.fail_pipeline_if_down = true;
    c.probe_interval_healthy  = std::chrono::milliseconds(5);
    c.probe_interval_degraded = std::chrono::milliseconds(2);
    c.degraded_max_failures   = 3;
    return c;
}

// Wait until subscribers have observed at least `n` events. Avoids
// races between bus dispatch and test assertions.
bool wait_for_events(const HealthSink& sink, std::size_t n,
                      std::chrono::milliseconds budget = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (sink.size() < n) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

} // namespace

TEST_CASE("ServiceMonitor: not configured stays NotConfigured and never probes") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    ServiceConfig cfg = make_cfg();
    cfg.health_url = "";
    ServiceMonitor mon(cfg, probe.fn(), bus);
    mon.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(mon.state() == ServiceState::NotConfigured);
    CHECK(probe.calls() == 0);
    CHECK(sink.size() == 0);
    mon.stop();
}

TEST_CASE("ServiceMonitor: first OK probe transitions Probing → Healthy") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    probe.enqueue_ok(8);
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    CHECK(mon.state() == ServiceState::Probing);
    mon.start();
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    CHECK(mon.state() == ServiceState::Healthy);
    REQUIRE(wait_for_events(sink, 1));
    auto evs = sink.snapshot();
    CHECK(evs.front().service == "llm");
    CHECK(evs.front().state == HealthState::Healthy);
    mon.stop();
}

TEST_CASE("ServiceMonitor: failure < threshold yields Degraded; recovery returns Healthy") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    probe.enqueue_ok(1);
    probe.enqueue_fail(1);
    probe.enqueue_ok(8);
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();
    // Probe 1: ok → Healthy
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    CHECK(mon.state() == ServiceState::Healthy);
    // Probe 2: fail → Degraded (1 of 3)
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    CHECK(mon.state() == ServiceState::Degraded);
    // Probe 3: ok → back to Healthy
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    CHECK(mon.state() == ServiceState::Healthy);

    REQUIRE(wait_for_events(sink, 3));
    auto evs = sink.snapshot();
    CHECK(evs[0].state == HealthState::Healthy);
    CHECK(evs[1].state == HealthState::Degraded);
    CHECK(evs[2].state == HealthState::Healthy);
    mon.stop();
}

TEST_CASE("ServiceMonitor: ≥ degraded_max_failures consecutive failures → Unhealthy") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    probe.enqueue_ok(1);
    probe.enqueue_fail(5); // 3 max — first two are Degraded, third+ Unhealthy
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();

    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500))); // ok
    CHECK(mon.state() == ServiceState::Healthy);
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500))); // fail 1
    CHECK(mon.state() == ServiceState::Degraded);
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500))); // fail 2
    CHECK(mon.state() == ServiceState::Degraded);
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500))); // fail 3 — crosses
    CHECK(mon.state() == ServiceState::Unhealthy);

    REQUIRE(wait_for_events(sink, 3));   // Healthy, Degraded, Unhealthy
    auto evs = sink.snapshot();
    CHECK(evs.back().state == HealthState::Unhealthy);
    mon.stop();
}

TEST_CASE("ServiceMonitor: Unhealthy → Healthy on a single OK probe") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    probe.enqueue_fail(3);
    probe.enqueue_ok(3);
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    }
    CHECK(mon.state() == ServiceState::Unhealthy);

    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    CHECK(mon.state() == ServiceState::Healthy);
    mon.stop();
}

TEST_CASE("ServiceMonitor: oscillating Healthy↔Degraded does not flip to Unhealthy") {
    EventBus bus;
    HealthSink sink(bus);
    MockProbe probe;
    probe.enqueue_ok(1);
    probe.enqueue_fail(1);   // → Degraded
    probe.enqueue_ok(1);     // → Healthy
    probe.enqueue_fail(1);   // → Degraded
    probe.enqueue_ok(1);     // → Healthy
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();
    for (int i = 0; i < 5; ++i) {
        REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    }
    CHECK(mon.state() == ServiceState::Healthy);
    auto snap = mon.snapshot();
    CHECK(snap.consecutive_failures == 0);
    // 4 transitions: H, D, H, D, H — but only edges count.
    REQUIRE(wait_for_events(sink, 5));
    mon.stop();
}

TEST_CASE("ServiceMonitor: snapshot exposes counters and timestamps") {
    EventBus bus;
    MockProbe probe;
    probe.enqueue_ok(3);
    probe.enqueue_fail(1);
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    }
    auto s = mon.snapshot();
    CHECK(s.name == "llm");
    CHECK(s.total_probes == 4);
    CHECK(s.total_failures == 1);
    CHECK(s.consecutive_failures == 1);
    CHECK(s.fail_pipeline_if_down);
    CHECK(s.last_ok_at.time_since_epoch().count() > 0);
    mon.stop();
}

TEST_CASE("ServiceMonitor: stop is idempotent and joins cleanly") {
    EventBus bus;
    MockProbe probe;
    probe.enqueue_ok(1);
    ServiceMonitor mon(make_cfg(), probe.fn(), bus);
    mon.start();
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    mon.stop();
    mon.stop();   // second call must be a no-op, not deadlock
    CHECK(mon.state() == ServiceState::Healthy);
}

TEST_CASE("ServiceMonitor: probe-fn exception is recorded as a failure") {
    EventBus bus;
    HealthSink sink(bus);
    auto throwing = [n = 0](std::string_view) mutable -> ProbeResult {
        if (++n <= 3) throw std::runtime_error("bang");
        return ProbeResult{
            .ok = true, .http_status = 200,
            .latency = std::chrono::milliseconds(0),
            .body_excerpt = {},
        };
    };
    ServiceMonitor mon(make_cfg(), throwing, bus);
    mon.start();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500)));
    }
    CHECK(mon.state() == ServiceState::Unhealthy);
    REQUIRE(mon.wait_for_probe(std::chrono::milliseconds(500))); // recovery
    CHECK(mon.state() == ServiceState::Healthy);
    mon.stop();
}
