#pragma once

#include "config/config.hpp"
#include "event/bus.hpp"
#include "metrics/registry.hpp"
#include "supervisor/supervisor.hpp"

#include <memory>
#include <vector>

namespace acva::orchestrator {

// Build the supervision stack: shared HttpProbe, Supervisor with the
// llm/stt/tts services registered, plus the bus subscriptions that
// mirror health + pipeline state into prometheus gauges. The
// Supervisor is returned by unique_ptr so the caller controls
// lifetime (stop before bus shutdown). Subscription handles are
// appended to `subscription_keepalive` so the bus retains them for
// the duration of the run.
//
// Calls supervisor->start() before returning so probes begin
// immediately.
[[nodiscard]] std::unique_ptr<supervisor::Supervisor>
build_supervisor(const config::Config& cfg,
                  event::EventBus& bus,
                  const std::shared_ptr<metrics::Registry>& registry,
                  std::vector<event::SubscriptionHandle>& subscription_keepalive);

} // namespace acva::orchestrator
