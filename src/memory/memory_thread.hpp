#pragma once

#include "event/queue.hpp"
#include "memory/db.hpp"
#include "memory/repository.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace acva::memory {

// MemoryThread: owns the SQLite Database and runs all reads/writes on a
// single dedicated thread. Callers post lambdas that receive a Repository
// reference; the lambdas execute serially in posting order.
//
// post() is fire-and-forget for writes; submit() returns a future for
// callers that need the result. Both go through the same in-order queue,
// so causality is preserved.
//
// Reads can use submit<T>() with a result-returning lambda, or the
// blocking read<T>() helper.
class MemoryThread {
public:
    using WriteJob = std::function<void(Repository&)>;

    [[nodiscard]] static Result<std::unique_ptr<MemoryThread>>
    open(const std::filesystem::path& path, std::size_t queue_capacity);

    ~MemoryThread();

    MemoryThread(const MemoryThread&) = delete;
    MemoryThread& operator=(const MemoryThread&) = delete;
    MemoryThread(MemoryThread&&) = delete;
    MemoryThread& operator=(MemoryThread&&) = delete;

    // Post a write/read for fire-and-forget execution. Returns false if the
    // queue is full (DropNewest policy) or the thread is shutting down.
    bool post(WriteJob job);

    // Submit a job and get its result back via future. The lambda runs on
    // the memory thread; the future is fulfilled when it returns.
    template <class Fn>
    auto submit(Fn&& fn) {
        using R = std::invoke_result_t<Fn, Repository&>;
        auto promise = std::make_shared<std::promise<R>>();
        auto future = promise->get_future();
        // Copy-capture the shared_ptr so the outer `promise` stays valid for
        // the !accepted shutdown path below. Moving it into the lambda would
        // leave the outer in a null moved-from state and turn the shutdown
        // path's `*promise` into UB.
        const bool accepted = post([promise,
                                    fn = std::forward<Fn>(fn)](Repository& repo) mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn(repo);
                    promise->set_value();
                } else {
                    promise->set_value(fn(repo));
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        if (!accepted) {
            // Fulfill with an exception so callers waiting on the future
            // don't hang on shutdown.
            promise_set_shutdown(*promise);
        }
        return future;
    }

    // Blocking read. Equivalent to submit(fn).get() but reads more naturally
    // at call sites that just want the value.
    template <class Fn>
    auto read(Fn&& fn) {
        return submit(std::forward<Fn>(fn)).get();
    }

    // Total drops since startup; useful for metrics.
    [[nodiscard]] std::uint64_t drops() const noexcept { return queue_.drops(); }
    [[nodiscard]] std::size_t depth() const noexcept { return queue_.size(); }

private:
    MemoryThread(Database db, std::size_t queue_capacity);

    void run();

    template <class P>
    static void promise_set_shutdown(P& promise) {
        try {
            throw std::runtime_error("memory thread shutting down");
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    }

    Database db_;
    Repository repo_;
    event::BoundedQueue<WriteJob> queue_;
    std::atomic<bool> running_{true};
    std::thread worker_;
};

} // namespace acva::memory
