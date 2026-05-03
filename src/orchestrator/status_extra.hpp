#pragma once

#include "orchestrator/capture_stack.hpp"
#include "supervisor/supervisor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace acva::orchestrator {

// Build the /status JSON-extra closure: pipeline_state +
// services[] + apm{} block. Reads supervisor + (lazily) the
// capture stack's APM stats on every request.
//
// Both arguments are taken by reference and captured into the
// returned closure — they MUST outlive the closure (i.e. outlive
// the ControlServer that holds it). Use `capture` by reference to
// the unique_ptr (not the underlying object) so the lambda picks
// up the eventual capture stack even if it's built AFTER the
// closure is created — that's the production order.
[[nodiscard]] std::function<std::string()>
make_status_extra(const supervisor::Supervisor& sup,
                   const std::unique_ptr<CaptureStack>& capture);

} // namespace acva::orchestrator
