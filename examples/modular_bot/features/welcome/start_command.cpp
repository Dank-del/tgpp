#include "start_command.hpp"

namespace example {
void StartCommand::handle(tgpp::ext::Context& context) {
    context.reply("Welcome! In a private chat, use /add Buy groceries and /tasks.");
}
} // namespace example
