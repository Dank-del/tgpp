#include <tgpp/ext/concurrent.hpp>

namespace tgpp::ext {
std::uint64_t chat_ordering_key(const Update& update) noexcept {
    for (const auto* message : {&update.message,
                                &update.edited_message,
                                &update.channel_post,
                                &update.edited_channel_post,
                                &update.business_message,
                                &update.edited_business_message,
                                &update.guest_message}) {
        if (*message) {
            return static_cast<std::uint64_t>((*message)->chat.id);
        }
    }
    if (update.callback_query && update.callback_query->message) {
        const auto& message = update.callback_query->message->value;
        if (const auto* accessible = std::get_if<Box<Message>>(&message);
            accessible && *accessible) {
            return static_cast<std::uint64_t>((*accessible)->chat.id);
        }
        if (const auto* inaccessible = std::get_if<InaccessibleMessage>(&message)) {
            return static_cast<std::uint64_t>(inaccessible->chat.id);
        }
    }
    if (update.chat_member) {
        return static_cast<std::uint64_t>(update.chat_member->chat.id);
    }
    if (update.my_chat_member) {
        return static_cast<std::uint64_t>(update.my_chat_member->chat.id);
    }
    if (update.chat_join_request) {
        return static_cast<std::uint64_t>(update.chat_join_request->chat.id);
    }
    if (update.message_reaction) {
        return static_cast<std::uint64_t>(update.message_reaction->chat.id);
    }
    if (update.message_reaction_count) {
        return static_cast<std::uint64_t>(update.message_reaction_count->chat.id);
    }
    if (update.deleted_business_messages) {
        return static_cast<std::uint64_t>(update.deleted_business_messages->chat.id);
    }
    return 0;
}

std::size_t ConcurrentApplication::lane(const Update& update) const {
    return (options_.key ? options_.key(update) : chat_ordering_key(update)) % workers_.size();
}

ConcurrentApplication::ConcurrentApplication(Factory factory, ConcurrencyOptions options)
    : options_(std::move(options)) {
    if (!factory || !options_.workers || options_.workers > 256 ||
        !options_.queue_capacity_per_worker) {
        throw std::invalid_argument(
            "concurrency needs a factory, 1-256 workers, and a positive queue capacity");
    }
    workers_.reserve(options_.workers);
    try {
        for (std::size_t i = 0; i < options_.workers; ++i) {
            auto worker = std::make_unique<Worker>();
            worker->application = factory();
            if (!worker->application) {
                throw std::invalid_argument("application factory returned null");
            }
            worker->application->start();
            workers_.push_back(std::move(worker));
        }
        for (auto& worker : workers_) {
            worker->thread = std::thread([this, value = worker.get()] { work(*value); });
        }
    } catch (...) {
        request_stop();
        for (auto& worker : workers_) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            } else {
                worker->application->stop();
            }
        }
        throw;
    }
}

ConcurrentApplication::~ConcurrentApplication() {
    shutdown();
}

bool ConcurrentApplication::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !closing_;
}

void ConcurrentApplication::request_stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    for (auto& worker : workers_) {
        worker->ready.notify_all();
        worker->space.notify_all();
    }
}

void ConcurrentApplication::shutdown() {
    for (const auto& worker : workers_) {
        if (worker->thread.joinable() && worker->thread.get_id() == std::this_thread::get_id()) {
            throw std::logic_error(
                "use request_stop() from a handler; join from an external thread");
        }
    }
    request_stop();
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

std::future<Result> ConcurrentApplication::submit(Update update) {
    auto& worker = *workers_[lane(update)];
    std::unique_lock<std::mutex> lock(mutex_);
    worker.space.wait(
        lock, [&] { return closing_ || worker.queue.size() < options_.queue_capacity_per_worker; });
    if (closing_) {
        throw std::logic_error("concurrent application is stopping");
    }
    worker.queue.push_back({std::move(update), {}});
    auto result = worker.queue.back().promise.get_future();
    lock.unlock();
    worker.ready.notify_one();
    return result;
}

std::optional<std::future<Result>> ConcurrentApplication::try_submit(Update& update) {
    auto& worker = *workers_[lane(update)];
    std::unique_lock<std::mutex> lock(mutex_);
    if (closing_ || worker.queue.size() >= options_.queue_capacity_per_worker) {
        return {};
    }
    worker.queue.push_back({std::move(update), {}});
    auto result = worker.queue.back().promise.get_future();
    lock.unlock();
    worker.ready.notify_one();
    return result;
}

void ConcurrentApplication::work(Worker& worker) noexcept {
    while (true) {
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            worker.ready.wait(lock, [&] { return closing_ || !worker.queue.empty(); });
            if (worker.queue.empty()) {
                break;
            }
            job.emplace(std::move(worker.queue.front()));
            worker.queue.pop_front();
        }
        worker.space.notify_one();
        try {
            job->promise.set_value(worker.application->process(job->update));
        } catch (...) {
            job->promise.set_exception(std::current_exception());
        }
    }
    worker.application->stop();
}
} // namespace tgpp::ext
