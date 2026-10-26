#pragma once
#include "catalog.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace showcase {
class Service {
    const tgpp::Bot& bot_;
    std::int64_t chat_;
    std::filesystem::path assets_, report_path_;
    Catalog catalog_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> jobs_;
    std::string active_;
    bool stopping_ = false;
    bool cancelling_ = false;
    tgpp::Json report_ = tgpp::Json::array();
    void work() noexcept;
    void execute(const std::string& job);
    void wait(std::chrono::milliseconds duration);
    void record(tgpp::Json result);
    void show_report(DemoContext& context);

  public:
    Service(const tgpp::Bot& bot,
            std::int64_t chat,
            std::filesystem::path assets,
            std::filesystem::path report_path);
    ~Service();
    void start();
    void stop() noexcept;
    bool enqueue(const std::string& job);
    void cancel();
    bool idle() const;

    bool knows(const std::string& id) const {
        return catalog_.find(id) != nullptr;
    }

    const Catalog& catalog() const noexcept {
        return catalog_;
    }

    tgpp::Json report() const;
};

// Each private chat gets its own service, worker, cancellation, and report.
// Bound live workers; evict an idle session when another chat needs a slot.
class Sessions {
    const tgpp::Bot& bot_;
    std::filesystem::path assets_, reports_;
    std::size_t capacity_;
    std::unordered_map<std::int64_t, std::shared_ptr<Service>> services_;
    bool started_ = false;
    bool stopped_ = false;

  public:
    Sessions(const tgpp::Bot& bot,
             std::filesystem::path assets,
             std::filesystem::path reports,
             std::size_t capacity = 8);
    ~Sessions();
    void start();
    void stop() noexcept;
    std::shared_ptr<Service> get(std::int64_t chat);
};
} // namespace showcase
