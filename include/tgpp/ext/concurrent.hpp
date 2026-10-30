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
    struct Job {
        Update update;
        std::promise<Result> promise;

        explicit Job(const Update& value) : update(value) {}

        explicit Job(Update&& value) : update(std::move(value)) {}
    };

    struct Worker {
        std::unique_ptr<Application> application;
        std::deque<std::unique_ptr<Job>> queue;
        std::thread thread;
        std::condition_variable ready, space;
    };

    std::vector<std::unique_ptr<Worker>> workers_;
    ConcurrencyOptions options_;
    mutable std::mutex mutex_;
    bool closing_ = false;
    std::size_t lane(const Update& update) const;
    std::future<Result> submit_owned(std::unique_ptr<Job> job);
    void work(Worker& worker) noexcept;

  public:
    explicit ConcurrentApplication(Factory factory, ConcurrencyOptions options = {});
    ~ConcurrentApplication();
    ConcurrentApplication(const ConcurrentApplication&) = delete;
    ConcurrentApplication& operator=(const ConcurrentApplication&) = delete;
    // Copies lvalues or moves rvalues into owned storage, blocking only when
    // their lane is full. Large generated updates never occupy a worker stack.
    std::future<Result> submit(const Update& update);
    std::future<Result> submit(Update&& update);
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
