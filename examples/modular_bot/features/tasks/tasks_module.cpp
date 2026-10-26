#include "tasks_module.hpp"
#include "handlers/add_task_command.hpp"
#include "handlers/list_tasks_command.hpp"

namespace example::tasks {
TasksModule::TasksModule(std::shared_ptr<TaskService> service) : service_(std::move(service)) {
    if (!service_) throw std::invalid_argument("task service must not be null");
}
void TasksModule::configure(tgpp::ext::Router& router) {
    auto& private_chats = router.child("private", tgpp::ext::filters::private_chat());
    private_chats.add<AddTaskCommand>(service_);
    private_chats.add<ListTasksCommand>(service_);
}
} // namespace example::tasks
