#pragma once

#include "event/event.hpp" // for TurnId alias

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace lclva::dialogue {

using event::TurnId;
using event::kNoTurn;

// CancellationToken: a single bool flipped from one direction. Workers
// holding a shared_ptr<CancellationToken> check is_cancelled() at safe
// points and unwind cleanly when set.
class CancellationToken {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> cancelled_{false};
};

// TurnContext: per-turn handle. Carried by every long-running operation
// (LLM stream, TTS request, playback enqueue) so that on UserInterrupted
// the orchestrator can invalidate every in-flight piece of work in one
// step (turn id bump + token cancel).
struct TurnContext {
    TurnId id = kNoTurn;
    std::shared_ptr<CancellationToken> token;
    std::chrono::steady_clock::time_point started_at{};

    [[nodiscard]] bool valid() const noexcept { return id != kNoTurn && token != nullptr; }
    [[nodiscard]] bool cancelled() const noexcept {
        return !token || token->is_cancelled();
    }
};

// Mints monotonically increasing turn ids and fresh cancellation tokens.
// A single TurnFactory owns the counter for the lifetime of the process;
// turn ids are unique within a session but may restart at 1 across restarts.
class TurnFactory {
public:
    [[nodiscard]] TurnContext mint();

private:
    std::atomic<TurnId> next_{1};
};

} // namespace lclva::dialogue
