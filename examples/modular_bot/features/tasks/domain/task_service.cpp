#include "task_service.hpp"
#include <stdexcept>

namespace example::tasks {
TaskService::TaskService(std::shared_ptr<TaskRepository> repository)
    : repository_(std::move(repository)) {
    if (!repository_) {
        throw std::invalid_argument("task repository must not be null");
    }
}

Task TaskService::add(std::int64_t user_id, std::string title) {
    if (user_id <= 0) {
        throw std::invalid_argument("A user account is required.");
    }
    const auto first = title.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        throw std::invalid_argument("Usage: /add Buy groceries");
    }
    title = title.substr(first, title.find_last_not_of(" \t\r\n") - first + 1);
    if (title.size() > 200) {
        throw std::invalid_argument("That task title is too long.");
    }
    return repository_->add(user_id, std::move(title));
}

std::vector<Task> TaskService::list(std::int64_t user_id) const {
    if (user_id <= 0) {
        throw std::invalid_argument("A user account is required.");
    }
    return repository_->list(user_id);
}
} // namespace example::tasks
