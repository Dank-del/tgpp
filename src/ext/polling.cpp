#include <limits>
#include <tgpp/ext/polling.hpp>

namespace tgpp::ext {
namespace {
void validate(const PollingOptions& options) {
    if (options.request.limit && (*options.request.limit < 1 || *options.request.limit > 100)) {
        throw std::invalid_argument("polling limit must be between 1 and 100");
    }
    if (options.request.timeout && *options.request.timeout < 0) {
        throw std::invalid_argument("polling timeout must not be negative");
    }
    if (options.request.offset && *options.request.offset < 0) {
        throw std::invalid_argument(
            "negative polling offsets discard updates; use an explicit Bot API request instead");
    }
}
} // namespace

Polling::Polling(Application& application, PollingOptions options)
    : application_(&application), bot_(&application.bot()), options_(std::move(options)),
      offset_(options_.request.offset) {
    validate(options_);
}

Polling::Polling(ConcurrentApplication& application, const Bot& bot, PollingOptions options)
    : concurrent_(&application), bot_(&bot), options_(std::move(options)),
      offset_(options_.request.offset) {
    validate(options_);
}

std::size_t Polling::poll(const std::function<bool()>& keep_running) {
    auto should_continue = [&] {
        return !stop_requested_.load() && (!keep_running || keep_running());
    };
    if (!should_continue()) {
        return 0;
    }
    if (application_) {
        application_->start();
    }
    auto request = options_.request;
    request.offset = offset_;
    auto updates = bot_->get_updates(request);
    std::size_t count = 0;
    std::vector<std::pair<std::int64_t, std::future<Result>>> pending;
    if (concurrent_) {
        pending.reserve(updates.size());
    }
    std::exception_ptr submission_error;
    auto submitted_offset = offset_;
    for (auto& update : updates) {
        if (!should_continue()) {
            break;
        }
        const auto minimum = concurrent_ ? submitted_offset : offset_;
        if (minimum && update.update_id < *minimum) {
            continue;
        }
        if (update.update_id < 0 || update.update_id == std::numeric_limits<std::int64_t>::max()) {
            if (!concurrent_) {
                throw std::invalid_argument("invalid update identifier");
            }
            submission_error =
                std::make_exception_ptr(std::invalid_argument("invalid update identifier"));
            break;
        }
        if (concurrent_) {
            const auto id = update.update_id;
            try {
                pending.emplace_back(id, concurrent_->submit(std::move(update)));
                submitted_offset = id + 1;
            } catch (...) {
                submission_error = std::current_exception();
                break;
            }
            continue;
        }
        application_->process(update);
        // Advance only after successful dispatch or an explicit ErrorAction::skip.
        offset_ = update.update_id + 1;
        ++count;
        if (!application_->is_running()) {
            break;
        }
    }
    // Wait for accepted work even on failure. Only a contiguous successful
    // prefix is acknowledged; later side effects may replay on restart.
    std::exception_ptr processing_error;
    for (auto& item : pending) {
        try {
            item.second.get();
            if (!processing_error) {
                offset_ = item.first + 1;
                ++count;
            }
        } catch (...) {
            if (!processing_error) {
                processing_error = std::current_exception();
            }
        }
    }
    if (processing_error) {
        std::rethrow_exception(processing_error);
    }
    if (submission_error) {
        std::rethrow_exception(submission_error);
    }
    return count;
}

std::size_t Polling::poll_once() {
    return poll({});
}

void Polling::run(const std::function<bool()>& keep_running) {
    auto running = [&] {
        return application_ ? application_->is_running() : concurrent_->is_running();
    };
    auto shutdown = [&] {
        if (application_) {
            application_->stop();
        } else {
            concurrent_->shutdown();
        }
    };
    try {
        if (application_ && !stop_requested_.load() && (!keep_running || keep_running())) {
            application_->start();
        }
        while (running() && !stop_requested_.load() && (!keep_running || keep_running())) {
            poll(keep_running);
        }
    } catch (...) {
        shutdown();
        throw;
    }
    shutdown();
}
} // namespace tgpp::ext
