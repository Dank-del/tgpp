#include <tgpp/ext/handler.hpp>
#include <algorithm>

namespace tgpp::ext {
namespace {
std::string normalize_command(std::string command) {
    if (!command.empty() && command.front() == '/') command.erase(0, 1);
    if (command.empty() || command.size() > 32 || !std::all_of(command.begin(), command.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    })) throw std::invalid_argument("command must contain 1-32 ASCII letters, digits, or underscores");
    for (auto& c : command) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return command;
}
void require_filter(const Filter& filter) {
    if (!filter) throw std::invalid_argument("filter must not be empty");
}
} // namespace
Handler::Handler(std::string name) : name_(std::move(name)) {
    if (name_.empty()) throw std::invalid_argument("handler name must not be empty");
}
CommandHandler::CommandHandler(std::string command)
    : Handler("command:/" + normalize_command(command)), command_(normalize_command(std::move(command))) {}
bool CommandHandler::matches(const Context& context) const {
    return context.command() && context.command()->name == command_;
}
CallbackQueryHandler::CallbackQueryHandler(std::string prefix)
    : Handler("callback:" + prefix), prefix_(std::move(prefix)) {}
bool CallbackQueryHandler::matches(const Context& context) const {
    const auto* callback = context.callback_query();
    return callback && callback->data && callback->data->compare(0, prefix_.size(), prefix_) == 0;
}
FunctionHandler::FunctionHandler(std::string name, Filter filter, Callback callback)
    : Handler(std::move(name)), filter_(std::move(filter)), callback_(std::move(callback)) {
    require_filter(filter_);
    if (!callback_) throw std::invalid_argument("handler callback must not be empty");
}
bool FunctionHandler::matches(const Context& context) const { return filter_(context); }
void FunctionHandler::handle(Context& context) { callback_(context); }

namespace filters {
Filter all() { return [](const Context&) { return true; }; }
Filter message() { return [](const Context& context) { return context.message() != nullptr; }; }
Filter text() { return [](const Context& context) { return context.message() && context.message()->text.has_value(); }; }
Filter private_chat() { return [](const Context& context) { return context.chat() && context.chat()->type == "private"; }; }
Filter command(std::string name) {
    return [name = normalize_command(std::move(name))](const Context& context) { return context.command() && context.command()->name == name; };
}
Filter callback_prefix(std::string prefix) {
    return [prefix = std::move(prefix)](const Context& context) {
        const auto* query = context.callback_query();
        return query && query->data && query->data->compare(0, prefix.size(), prefix) == 0;
    };
}
Filter both(Filter first, Filter second) {
    require_filter(first); require_filter(second);
    return [first = std::move(first), second = std::move(second)](const Context& context) { return first(context) && second(context); };
}
Filter either(Filter first, Filter second) {
    require_filter(first); require_filter(second);
    return [first = std::move(first), second = std::move(second)](const Context& context) { return first(context) || second(context); };
}
Filter negate(Filter filter) {
    require_filter(filter);
    return [filter = std::move(filter)](const Context& context) { return !filter(context); };
}
} // namespace filters
} // namespace tgpp::ext
