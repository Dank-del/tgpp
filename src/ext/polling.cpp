#include <tgpp/ext/polling.hpp>
#include <limits>

namespace tgpp::ext {
Polling::Polling(Application& application, PollingOptions options)
    : application_(application), options_(std::move(options)), offset_(options_.request.offset) {
    if (options_.request.limit && (*options_.request.limit < 1 || *options_.request.limit > 100))
        throw std::invalid_argument("polling limit must be between 1 and 100");
    if (options_.request.timeout && *options_.request.timeout < 0)
        throw std::invalid_argument("polling timeout must not be negative");
    if (offset_ && *offset_ < 0)
        throw std::invalid_argument("negative polling offsets discard updates; use an explicit Bot API request instead");
}
std::size_t Polling::poll(const std::function<bool()>& keep_running) {
    auto should_continue = [&] { return !stop_requested_.load() && (!keep_running || keep_running()); };
    if (!should_continue()) return 0;
    application_.start();
    auto request = options_.request;
    request.offset = offset_;
    auto updates = application_.bot().get_updates(request);
    std::size_t count = 0;
    for (const auto& update : updates) {
        if (!should_continue()) break;
        if (offset_ && update.update_id < *offset_) continue;
        if (update.update_id < 0 || update.update_id == std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("invalid update identifier");
        application_.process(update);
        // Advance only after successful dispatch or an explicit ErrorAction::skip.
        offset_ = update.update_id + 1;
        ++count;
        if (!application_.is_running()) break;
    }
    return count;
}
std::size_t Polling::poll_once() { return poll({}); }
void Polling::run(const std::function<bool()>& keep_running) {
    try {
        if (!stop_requested_.load() && (!keep_running || keep_running())) application_.start();
        while (application_.is_running() && !stop_requested_.load() && (!keep_running || keep_running())) poll(keep_running);
    } catch (...) {
        application_.stop();
        throw;
    }
    application_.stop();
}
} // namespace tgpp::ext
