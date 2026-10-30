#include "service.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace showcase {
Service::Service(const tgpp::Bot& bot,
                 std::int64_t chat,
                 std::filesystem::path assets,
                 std::filesystem::path report_path)
    : bot_(bot), chat_(chat), assets_(std::move(assets)), report_path_(std::move(report_path)),
      catalog_(make_catalog()) {
    if (chat_ <= 0) {
        throw std::invalid_argument("showcase needs a private chat ID");
    }
    if (!report_path_.empty() && std::filesystem::is_regular_file(report_path_)) {
        std::ifstream input(report_path_);
        const auto saved = tgpp::Json::parse(input);
        if (!saved.is_array() || saved.size() > catalog_.demos().size()) {
            throw std::runtime_error("invalid saved showcase report");
        }
        for (const auto& result : saved) {
            if (!result.is_object() || !result.contains("id") || !result.at("id").is_string() ||
                !catalog_.find(result.at("id").get<std::string>()) || !result.contains("title") ||
                !result.at("title").is_string() || !result.contains("status") ||
                (result.at("status") != "passed" && result.at("status") != "failed")) {
                throw std::runtime_error("invalid saved showcase result");
            }
        }
        report_ = saved;
    }
}

Service::~Service() {
    stop();
}

void Service::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_.joinable()) {
        return;
    }
    if (stopping_) {
        throw std::logic_error("showcase service is stopped");
    }
    worker_ = std::thread([this] { work(); });
}

void Service::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool Service::enqueue(const std::string& job) {
    if (job != "tour" && job != "menu" && job != "report" && !catalog_.find(job)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= 3 || active_ == job ||
        std::find(jobs_.begin(), jobs_.end(), job) != jobs_.end()) {
        return false;
    }
    jobs_.push_back(job);
    wake_.notify_all();
    return true;
}

void Service::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelling_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
}

bool Service::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.empty() && jobs_.empty();
}

void Service::wait(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (wake_.wait_for(lock, duration, [this] { return stopping_ || cancelling_; })) {
        throw Cancelled();
    }
}

tgpp::Json Service::report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}

void Service::record(tgpp::Json result) {
    tgpp::Json snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = std::find_if(report_.begin(), report_.end(), [&](const auto& entry) {
            return entry.at("id") == result.at("id");
        });
        if (existing == report_.end()) {
            report_.push_back(result);
        } else {
            *existing = result;
        }
        snapshot = report_;
    }
    std::cout << result.dump() << std::endl;
    if (!report_path_.empty()) {
        if (!report_path_.parent_path().empty()) {
            std::filesystem::create_directories(report_path_.parent_path());
        }
        auto temporary = report_path_;
        temporary += ".tmp";
        {
            std::ofstream output(temporary);
            if (!output) {
                throw std::runtime_error("cannot write showcase report");
            }
            output << snapshot.dump(2) << '\n';
        }
        std::filesystem::rename(temporary, report_path_);
    }
}

void Service::show_report(DemoContext& context) {
    const auto results = report();
    std::ostringstream text;
    text << "tgpp showcase report\n\n";
    if (results.empty()) {
        text << "No demos have run yet. Use /tour or pick a button.";
    }
    for (const auto& result : results) {
        text << (result.at("status") == "passed" ? "✅ " : "⚠️ ")
             << result.at("title").get<std::string>();
        if (result.contains("error")) {
            text << ": " << result.at("error").get<std::string>().substr(0, 110);
        }
        text << '\n';
    }
    text << "\nUse /showcase to run individual demos again.";
    context.text(text.str());
}

void Service::execute(const std::string& job) {
    DemoContext context(bot_, chat_, assets_, [this](auto duration) { wait(duration); });
    if (job == "menu") {
        tgpp::SendMessageRequest request{};
        request.chat_id = chat_;
        request.text = "tgpp · Telegram showcase\n\nPick a message type below, or /tour for the "
                       "whole tour. /stop cancels the tour; /report shows what worked.\n\nAll "
                       "media is bundled with the C++20 example. Newer features are tested "
                       "individually and reported if this bot/server cannot use them.";
        request.reply_markup = catalog_.keyboard();
        request.disable_notification = true;
        context.call([&] { return bot_.send_message(request); });
        return;
    }
    if (job == "report") {
        show_report(context);
        return;
    }
    if (job == "tour") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            report_ = tgpp::Json::array();
        }
        context.text("▶ Starting the tgpp message tour. You will see " +
                     std::to_string(catalog_.demos().size()) +
                     " demos. You can use /stop at any time.");
    }
    for (const auto& demo : catalog_.demos()) {
        if (job != "tour" && job != demo.id) {
            continue;
        }
        try {
            demo.run(context);
            record({{"id", demo.id},
                    {"title", demo.title},
                    {"group", demo.group},
                    {"status", "passed"}});
        } catch (const Cancelled&) {
            throw;
        } catch (const tgpp::ApiError& error) {
            record({{"id", demo.id},
                    {"title", demo.title},
                    {"group", demo.group},
                    {"status", "failed"},
                    {"error_code", error.code},
                    {"error", error.what()}});
        } catch (const std::exception& error) {
            record({{"id", demo.id},
                    {"title", demo.title},
                    {"group", demo.group},
                    {"status", "failed"},
                    {"error", error.what()}});
        }
    }
    if (job == "tour") {
        show_report(context);
    }
}

void Service::work() noexcept {
    while (true) {
        std::string job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            active_ = job;
            cancelling_ = false;
        }
        try {
            execute(job);
        } catch (const Cancelled&) {
            std::cout << "showcase job cancelled" << std::endl;
        } catch (const std::exception& error) {
            std::cerr << "showcase job failed: " << error.what() << std::endl;
        } catch (...) {
            std::cerr << "showcase job failed: unknown exception" << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_.clear();
        }
    }
}

Sessions::Sessions(const tgpp::Bot& bot,
                   std::filesystem::path assets,
                   std::filesystem::path reports,
                   std::size_t capacity)
    : bot_(bot), assets_(std::move(assets)), reports_(std::move(reports)), capacity_(capacity) {
    if (!capacity_) {
        throw std::invalid_argument("session capacity must be positive");
    }
}

Sessions::~Sessions() {
    stop();
}

void Sessions::start() {
    if (stopped_) {
        throw std::logic_error("showcase sessions are stopped");
    }
    started_ = true;
}

void Sessions::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    for (const auto& entry : services_) {
        entry.second->stop();
    }
    services_.clear();
}

std::shared_ptr<Service> Sessions::get(std::int64_t chat) {
    if (chat <= 0) {
        throw std::invalid_argument("showcase needs a positive private chat ID");
    }
    if (!started_ || stopped_) {
        throw std::logic_error("showcase sessions are not running");
    }
    if (const auto found = services_.find(chat); found != services_.end()) {
        return found->second;
    }
    if (services_.size() >= capacity_) {
        const auto idle = std::find_if(services_.begin(), services_.end(), [](const auto& entry) {
            return entry.second->idle();
        });
        if (idle == services_.end()) {
            return {};
        }
        idle->second->stop();
        services_.erase(idle);
    }
    auto service = std::make_shared<Service>(
        bot_,
        chat,
        assets_,
        reports_.empty() ? std::filesystem::path{} : reports_ / (std::to_string(chat) + ".json"));
    service->start();
    services_.emplace(chat, service);
    return service;
}
} // namespace showcase
