#include "dialogue/turn.hpp"

namespace acva::dialogue {

TurnContext TurnFactory::mint() {
    TurnContext ctx;
    ctx.id = next_.fetch_add(1, std::memory_order_relaxed);
    ctx.token = std::make_shared<CancellationToken>();
    ctx.started_at = std::chrono::steady_clock::now();
    return ctx;
}

} // namespace acva::dialogue
