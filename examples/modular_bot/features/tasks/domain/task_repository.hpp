#pragma once
#include "task.hpp"
#include <vector>

namespace example::tasks {
// Domain code depends on this interface. A database adapter can replace the
// example's in-memory implementation without changing handlers or services.
class TaskRepository {
public:
    virtual ~TaskRepository() = default;
    virtual Task add(std::int64_t user_id, std::string title) = 0;
    virtual std::vector<Task> list(std::int64_t user_id) const = 0;
};
} // namespace example::tasks
