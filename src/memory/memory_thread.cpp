#include "memory/memory_thread.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#include <utility>

namespace acva::memory {

Result<std::unique_ptr<MemoryThread>>
MemoryThread::open(const std::filesystem::path& path, std::size_t queue_capacity) {
    auto opened = Database::open(path);
    if (auto* err = std::get_if<DbError>(&opened)) {
        return *err;
    }
    auto db = std::move(std::get<Database>(opened));
    return std::unique_ptr<MemoryThread>{new MemoryThread(std::move(db), queue_capacity)};
}

MemoryThread::MemoryThread(Database db, std::size_t queue_capacity)
    : db_(std::move(db)),
      repo_(db_),
      queue_(queue_capacity, event::OverflowPolicy::DropNewest) {
    worker_ = std::thread([this] { run(); });
}

MemoryThread::~MemoryThread() {
    running_.store(false, std::memory_order_release);
    queue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool MemoryThread::post(WriteJob job) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }
    return queue_.push(std::move(job));
}

void MemoryThread::run() {
    log::info("memory", "memory thread started");
    while (running_.load(std::memory_order_acquire)) {
        auto job = queue_.pop();
        if (!job) {
            return; // closed and drained
        }
        try {
            (*job)(repo_);
        } catch (const std::exception& ex) {
            log::error("memory", fmt::format("job threw: {}", ex.what()));
        } catch (...) {
            log::error("memory", "job threw (non-std exception)");
        }
    }
}

} // namespace acva::memory
