#include "orchestrator/supervisor_setup.hpp"

#include "event/event.hpp"
#include "supervisor/probe.hpp"
#include "supervisor/service.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace acva::orchestrator {

std::unique_ptr<supervisor::Supervisor>
build_supervisor(const config::Config& cfg,
                  event::EventBus& bus,
                  const std::shared_ptr<metrics::Registry>& registry,
                  std::vector<event::SubscriptionHandle>& subscription_keepalive) {
    // One HttpProbe shared by every ServiceMonitor — it carries no
    // per-call state and uses the supervisor-wide probe_timeout_ms.
    // Wrapped in a shared_ptr so the ProbeFn closure keeps it alive
    // for the entire process lifetime.
    auto http_probe = std::make_shared<supervisor::HttpProbe>(
        std::chrono::milliseconds(cfg.supervisor.probe_timeout_ms));
    supervisor::ProbeFn probe_fn = [http_probe](std::string_view url) {
        return http_probe->get(url);
    };

    auto sup = std::make_unique<supervisor::Supervisor>(
        cfg.supervisor, bus, probe_fn);
    sup->register_service(
        supervisor::ServiceConfig::from_health("llm", cfg.llm.health));
    sup->register_service(
        supervisor::ServiceConfig::from_health("stt", cfg.stt.health));
    sup->register_service(
        supervisor::ServiceConfig::from_health("tts", cfg.tts.health));

    // Pre-create the per-service health gauges so /metrics shows the
    // full grid before the first probe lands.
    if (!cfg.llm.health.health_url.empty()) registry->register_service_for_health("llm");
    if (!cfg.stt.health.health_url.empty()) registry->register_service_for_health("stt");
    if (!cfg.tts.health.health_url.empty()) registry->register_service_for_health("tts");

    // Mirror HealthChanged events into the metric.
    {
        event::SubscribeOptions opts;
        opts.name           = "metrics.health";
        opts.queue_capacity = 64;
        opts.policy         = event::OverflowPolicy::DropOldest;
        subscription_keepalive.push_back(bus.subscribe<event::HealthChanged>(
            opts, [registry](const event::HealthChanged& e) {
                const char* s = "unknown";
                switch (e.state) {
                    case event::HealthState::Unknown:   s = "unknown";   break;
                    case event::HealthState::Healthy:   s = "healthy";   break;
                    case event::HealthState::Degraded:  s = "degraded";  break;
                    case event::HealthState::Unhealthy: s = "unhealthy"; break;
                }
                registry->set_health_state(e.service, s);
            }));
    }

    // Pipeline-state metric updater. Polls supervisor.pipeline_state()
    // on every event so the gauge stays current; cheap because both
    // calls are atomic loads.
    {
        event::SubscribeOptions opts;
        opts.name           = "metrics.pipeline";
        opts.queue_capacity = 64;
        opts.policy         = event::OverflowPolicy::DropOldest;
        auto* sup_ptr = sup.get();
        subscription_keepalive.push_back(bus.subscribe_all(
            opts, [registry, sup_ptr](const event::Event&) {
                registry->set_pipeline_state(std::string(
                    supervisor::to_string(sup_ptr->pipeline_state())).c_str());
            }));
    }

    sup->start();
    return sup;
}

} // namespace acva::orchestrator
