#include "add_task_command.hpp"

namespace example::tasks {
AddTaskCommand::AddTaskCommand(std::shared_ptr<TaskService> service)
    : CommandHandler("add"), service_(std::move(service)) {
    if (!service_) {
        throw std::invalid_argument("task service must not be null");
    }
}

void AddTaskCommand::handle(tgpp::ext::Context& context) {
    if (!context.user() || !context.command()) {
        throw std::logic_error("add task needs a user command");
    }
    Task task{};
    try {
        task = service_->add(context.user()->id, context.command()->argument_text);
    } catch (const std::invalid_argument& error) {
        context.reply(error.what());
        return;
    }
    context.reply("Added #" + std::to_string(task.id) + ": " + task.title);
}
} // namespace example::tasks
