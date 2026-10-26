#pragma once
#include "domain/task_service.hpp"
#include <tgpp/ext/module.hpp>

namespace example::tasks {
class TasksModule final : public tgpp::ext::Module {
    std::shared_ptr<TaskService> service_;
public:
    explicit TasksModule(std::shared_ptr<TaskService> service);
    std::string name() const override { return "tasks"; }
    void configure(tgpp::ext::Router& router) override;
};
} // namespace example::tasks
