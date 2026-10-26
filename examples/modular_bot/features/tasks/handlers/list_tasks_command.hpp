#pragma once
#include "features/tasks/domain/task_service.hpp"
#include <tgpp/ext/handler.hpp>

namespace example::tasks {
class ListTasksCommand final : public tgpp::ext::CommandHandler {
    std::shared_ptr<TaskService> service_;

  public:
    explicit ListTasksCommand(std::shared_ptr<TaskService> service);
    void handle(tgpp::ext::Context& context) override;
};
} // namespace example::tasks
