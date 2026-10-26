#pragma once
#include "application.hpp"
#include "concurrent.hpp"
#include <atomic>

namespace tgpp::ext {
struct PollingOptions {
    GetUpdatesRequest request;
    PollingOptions() { request.timeout = 30; request.limit = 100; }
};

class Polling {
    Application* application_ = nullptr;
    ConcurrentApplication* concurrent_ = nullptr;
    const Bot* bot_;
    PollingOptions options_;
    std::optional<std::int64_t> offset_;
    std::atomic<bool> stop_requested_{false};
    std::size_t poll(const std::function<bool()>& keep_running);
public:
    explicit Polling(Application& application, PollingOptions options = {});
    Polling(ConcurrentApplication& application, const Bot& bot, PollingOptions options = {});
    // Returns processed updates; exposes checkpoint for a persistent store.
    std::size_t poll_once();
    std::optional<std::int64_t> offset() const noexcept { return offset_; }
    // Only request_stop() is safe to call from a different thread. In a signal
    // handler use sig_atomic_t, then read it through run's keep_running callback.
    void request_stop() noexcept { stop_requested_.store(true); }
    void run(const std::function<bool()>& keep_running = {});
};
} // namespace tgpp::ext
