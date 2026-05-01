#include "supervisor/keep_alive.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>

namespace acva::supervisor {

KeepAlive::KeepAlive(Options opts) : opts_(std::move(opts)) {}

KeepAlive::~KeepAlive() {
    stop();
}

void KeepAlive::start() {
    if (!opts_.should_fire || !opts_.on_tick) return;       // misconfigured
    if (running_.exchange(true)) return;                    // already started
    stopping_.store(false, std::memory_order_release);
    worker_ = std::thread([this]{ run(); });
}

void KeepAlive::stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard lk(mu_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void KeepAlive::run() {
    log::info("keep_alive",
        fmt::format("starting; interval = {} ms", opts_.interval.count()));

    while (running_.load(std::memory_order_acquire)) {
        // Wait the interval, but bail out early on stop().
        {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, opts_.interval, [this]{
                return stopping_.load(std::memory_order_acquire);
            });
            if (stopping_.load(std::memory_order_acquire)) break;
        }

        bool should_fire = false;
        try {
            should_fire = opts_.should_fire();
        } catch (const std::exception& ex) {
            log::info("keep_alive",
                std::string{"should_fire threw: "} + ex.what());
            should_fire = false;
        } catch (...) {
            log::info("keep_alive", "should_fire threw unknown");
            should_fire = false;
        }

        if (!should_fire) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            if (opts_.on_skipped) opts_.on_skipped();
            continue;
        }

        try {
            opts_.on_tick();
        } catch (const std::exception& ex) {
            log::info("keep_alive",
                std::string{"on_tick threw: "} + ex.what());
        } catch (...) {
            log::info("keep_alive", "on_tick threw unknown");
        }
        fired_.fetch_add(1, std::memory_order_relaxed);
        if (opts_.on_fired) opts_.on_fired();
    }

    log::info("keep_alive", "stopped");
}

} // namespace acva::supervisor
