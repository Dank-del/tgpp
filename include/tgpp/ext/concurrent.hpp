#pragma once
#include "application.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace tgpp::ext {
struct ConcurrencyOptions {
    std::size_t workers = 4;
    std::size_t queue_capacity_per_worker = 256;
    // Equal keys always use the same FIFO worker. By default, chat ID is used;
    // chatless events share key zero. Override when ordering by another scope.
    std::function<std::uint64_t(const Update&)> key;
};

class ConcurrentApplication {
public:
    using Factory = std::function<std::unique_ptr<Application>()>;
private:
    struct Job { Update update; std::promise<Result> promise; };
    struct Worker {
        std::unique_ptr<Application> application;
        std::deque<Job> queue;
        std::thread thread;
        std::condition_variable ready, space;
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    ConcurrencyOptions options_;
    mutable std::mutex mutex_;
    bool closing_ = false;
    std::size_t lane(const Update& update) const;
    void work(Worker& worker) noexcept;
public:
    explicit ConcurrentApplication(Factory factory, ConcurrencyOptions options = {});
    ~ConcurrentApplication();
    ConcurrentApplication(const ConcurrentApplication&) = delete;
    ConcurrentApplication& operator=(const ConcurrentApplication&) = delete;
    // Takes ownership of the update and blocks only when its lane is full.
    std::future<Result> submit(Update update);
    // Moves from update only on acceptance. No allocation of a promise/future
    // is needed on the full-queue path; the caller retains the update to retry.
    std::optional<std::future<Result>> try_submit(Update& update);
    bool is_running() const;
    // Stops acceptance and drains accepted jobs. Safe from a handler/thread.
    void request_stop() noexcept;
    // Join from an external thread before destroying application dependencies.
    void shutdown();
};

std::uint64_t chat_ordering_key(const Update& update) noexcept;
} // namespace tgpp::ext
