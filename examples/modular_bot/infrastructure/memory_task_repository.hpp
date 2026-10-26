#pragma once
#include "features/tasks/domain/task_repository.hpp"
#include <unordered_map>

namespace example {
// Example persistence only; data is lost when the process exits.
class MemoryTaskRepository final : public tasks::TaskRepository {
    std::unordered_map<std::int64_t, std::vector<tasks::Task>> tasks_;
    std::int64_t next_id_ = 1;

  public:
    tasks::Task add(std::int64_t user_id, std::string title) override;
    std::vector<tasks::Task> list(std::int64_t user_id) const override;
};
} // namespace example
