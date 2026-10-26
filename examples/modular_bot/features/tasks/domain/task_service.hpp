#pragma once
#include "task_repository.hpp"
#include <memory>

namespace example::tasks {
// This service has no Telegram or tgpp dependency.
class TaskService {
    std::shared_ptr<TaskRepository> repository_;
public:
    explicit TaskService(std::shared_ptr<TaskRepository> repository);
    Task add(std::int64_t user_id, std::string title);
    std::vector<Task> list(std::int64_t user_id) const;
};
} // namespace example::tasks
