#include "dialogue/barge_in.hpp"

#include "audio/apm.hpp"
#include "log/log.hpp"

#include <fmt/format.h>

#include <cmath>
#include <utility>

namespace acva::dialogue {

namespace {

bool aec_active(const audio::Apm* apm) noexcept {
    return apm != nullptr && apm->aec_active();
}

bool erle_meets(const audio::Apm* apm, float threshold_db) noexcept {
    if (apm == nullptr) return false;
    const float erle = apm->erle_db();
    if (std::isnan(erle)) return false;
    return erle >= threshold_db;
}

} // namespace

BargeInDetector::BargeInDetector(event::EventBus& bus,
                                  const Fsm& fsm,
                                  const audio::Apm* apm,
                                  const config::BargeInConfig& cfg)
    : bus_(bus), fsm_(fsm), apm_(apm), cfg_(cfg) {}

BargeInDetector::~BargeInDetector() { stop(); }

void BargeInDetector::start() {
    if (sub_) return;
    if (!cfg_.enabled) {
        log::info("barge_in", "disabled by config; detector inactive");
        return;
    }
    event::SubscribeOptions opts;
    opts.name = "dialogue.barge_in";
    opts.queue_capacity = 64;
    // Drop oldest: a backlog of stale SpeechStarted events is no good
    // — the freshest one is the one we want to react to. Latency
    // matters more than completeness for this subscriber.
    opts.policy = event::OverflowPolicy::DropOldest;
    sub_ = bus_.subscribe<event::SpeechStarted>(
        std::move(opts),
        [this](const event::SpeechStarted& e) { on_speech_started(e); });
    log::info("barge_in", fmt::format(
        "active (require_aec_converged={}, min_erle={:.1f}dB, cooldown={}ms)",
        cfg_.require_aec_converged, cfg_.min_aec_erle_db,
        cfg_.cool_down_after_turn_ms));
}

void BargeInDetector::stop() {
    if (!sub_) return;
    sub_->stop();
    sub_.reset();
}

std::chrono::steady_clock::time_point BargeInDetector::last_fired_at() const noexcept {
    const auto ns = last_fired_ns_.load(std::memory_order_acquire);
    if (ns == 0) return {};
    return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{ns}};
}

bool BargeInDetector::aec_ok() {
    if (!cfg_.require_aec_converged) return true;
    if (!aec_active(apm_)) {
        suppressed_aec_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!erle_meets(apm_, cfg_.min_aec_erle_db)) {
        suppressed_aec_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool BargeInDetector::evaluate_for_test(const event::SpeechStarted& e) {
    on_speech_started(e);
    return last_fired_ns_.load(std::memory_order_acquire) != 0
        && last_fired_turn_.load(std::memory_order_acquire) == fsm_.snapshot().active_turn;
}

void BargeInDetector::on_speech_started(const event::SpeechStarted& /*e*/) {
    const auto snap = fsm_.snapshot();
    if (snap.state != State::Speaking) {
        return; // not a barge-in scenario
    }
    if (snap.active_turn == event::kNoTurn) {
        // Defensive: shouldn't happen — Speaking implies an active turn.
        return;
    }
    if (last_fired_turn_.load(std::memory_order_acquire) == snap.active_turn) {
        // Already fired for this turn; don't double-cancel.
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - snap.entered_speaking_at);
    if (since < std::chrono::milliseconds{cfg_.cool_down_after_turn_ms}) {
        suppressed_cooldown_.fetch_add(1, std::memory_order_relaxed);
        suppressed_total_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!aec_ok()) {
        suppressed_total_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    last_fired_turn_.store(snap.active_turn, std::memory_order_release);
    last_fired_ns_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count(),
        std::memory_order_release);
    fires_total_.fetch_add(1, std::memory_order_relaxed);

    log::info("barge_in",
        fmt::format("firing UserInterrupted: turn={} since_speaking={}ms",
                    snap.active_turn, since.count()));
    bus_.publish(event::UserInterrupted{
        .turn = snap.active_turn,
        .ts   = now,
    });
    if (on_fired_) {
        on_fired_(snap.active_turn, now);
    }
}

} // namespace acva::dialogue
