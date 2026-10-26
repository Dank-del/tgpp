#include "memory_task_repository.hpp"
#include <limits>
#include <stdexcept>

namespace example {
tasks::Task MemoryTaskRepository::add(std::int64_t user_id, std::string title) {
    if (next_id_ == std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("task identifiers exhausted");
    }
    tasks::Task task{next_id_, std::move(title)};
    tasks_[user_id].push_back(task);
    ++next_id_;
    return task;
}

std::vector<tasks::Task> MemoryTaskRepository::list(std::int64_t user_id) const {
    const auto found = tasks_.find(user_id);
    return found == tasks_.end() ? std::vector<tasks::Task>{} : found->second;
}
} // namespace example
