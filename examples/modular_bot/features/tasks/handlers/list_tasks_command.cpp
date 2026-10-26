#include "list_tasks_command.hpp"
#include <algorithm>
#include <sstream>

namespace example::tasks {
ListTasksCommand::ListTasksCommand(std::shared_ptr<TaskService> service)
    : CommandHandler("tasks"), service_(std::move(service)) {
    if (!service_) throw std::invalid_argument("task service must not be null");
}
void ListTasksCommand::handle(tgpp::ext::Context& context) {
    if (!context.user()) throw std::logic_error("list tasks needs a user");
    const auto tasks = service_->list(context.user()->id);
    if (tasks.empty()) { context.reply("No tasks yet. Try /add Buy groceries"); return; }
    std::ostringstream text;
    text << "Your tasks:\n";
    const auto count = std::min<std::size_t>(tasks.size(), 10);
    for (std::size_t i = 0; i < count; ++i) text << '#' << tasks[i].id << ": " << tasks[i].title << '\n';
    if (tasks.size() > count) text << "Showing " << count << " of " << tasks.size() << " tasks.";
    context.reply(text.str());
}
} // namespace example::tasks
