#include <algorithm>
#include <sstream>
#include <tgpp/ext/context.hpp>

namespace tgpp::ext {
namespace {
bool command_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::string lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return value;
}

std::optional<Command> parse_command(const Message* message, std::string_view username) {
    if (!message || !message->text || !message->entities) {
        return {};
    }
    const auto& text = *message->text;
    for (const auto& entity : *message->entities) {
        if (entity.type != "bot_command" || entity.offset != 0 || entity.length < 2) {
            continue;
        }
        if (std::uint64_t(entity.length) > text.size() || text[0] != '/') {
            return {};
        }
        if (std::uint64_t(entity.length) < text.size() &&
            text.find_first_of(" \t\r\n", static_cast<std::size_t>(entity.length)) !=
                static_cast<std::size_t>(entity.length)) {
            return {};
        }
        // Command tokens are ASCII: their byte length equals their UTF-16
        // length. Reject non-ASCII tokens instead of slicing Unicode by bytes.
        const auto token = text.substr(1, static_cast<std::size_t>(entity.length) - 1);
        const auto at = token.find('@');
        auto name = token.substr(0, at);
        if (name.empty() || name.size() > 32 ||
            !std::all_of(name.begin(), name.end(), command_char)) {
            return {};
        }
        if (at != std::string::npos) {
            const auto target = token.substr(at + 1);
            auto own = std::string(username);
            if (!own.empty() && own.front() == '@') {
                own.erase(0, 1);
            }
            if (target.empty() || !std::all_of(target.begin(), target.end(), command_char) ||
                own.empty() || lower(target) != lower(own)) {
                return {};
            }
        }
        auto arguments = text.substr(static_cast<std::size_t>(entity.length));
        auto first = arguments.find_first_not_of(" \t\r\n");
        arguments =
            first == std::string::npos
                ? ""
                : arguments.substr(first, arguments.find_last_not_of(" \t\r\n") - first + 1);
        Command command{lower(name), arguments, {}};
        std::istringstream stream(arguments);
        for (std::string argument; stream >> argument;) {
            command.arguments.push_back(std::move(argument));
        }
        return command;
    }
    return {};
}

const Message* callback_message(const CallbackQuery* query) noexcept {
    if (!query || !query->message) {
        return nullptr;
    }
    const auto* message = std::get_if<Box<Message>>(&query->message->value);
    return message && *message ? &**message : nullptr;
}
} // namespace

Context::Context(const Bot& bot, const Update& update, std::string_view username)
    : bot_(bot), update_(update), command_(parse_command(message(), username)) {}

const Message* Context::message() const noexcept {
    for (const auto* item : {&update_.message,
                             &update_.edited_message,
                             &update_.channel_post,
                             &update_.edited_channel_post,
                             &update_.business_message,
                             &update_.edited_business_message,
                             &update_.guest_message}) {
        if (*item) {
            return &**item;
        }
    }
    return nullptr;
}

const CallbackQuery* Context::callback_query() const noexcept {
    return update_.callback_query ? &*update_.callback_query : nullptr;
}

const User* Context::user() const noexcept {
    if (const auto* callback = callback_query()) {
        return &callback->from;
    }
    if (const auto* value = message()) {
        return value->from ? &*value->from : nullptr;
    }
    if (update_.inline_query) {
        return &update_.inline_query->from;
    }
    if (update_.chosen_inline_result) {
        return &update_.chosen_inline_result->from;
    }
    if (update_.shipping_query) {
        return &update_.shipping_query->from;
    }
    if (update_.pre_checkout_query) {
        return &update_.pre_checkout_query->from;
    }
    if (update_.chat_member) {
        return &update_.chat_member->from;
    }
    if (update_.my_chat_member) {
        return &update_.my_chat_member->from;
    }
    if (update_.chat_join_request) {
        return &update_.chat_join_request->from;
    }
    return nullptr;
}

const Chat* Context::chat() const noexcept {
    if (const auto* value = message()) {
        return &value->chat;
    }
    if (const auto* callback = callback_query(); callback && callback->message) {
        if (const auto* value = callback_message(callback)) {
            return &value->chat;
        }
        if (const auto* value = std::get_if<InaccessibleMessage>(&callback->message->value)) {
            return &value->chat;
        }
    }
    if (update_.chat_member) {
        return &update_.chat_member->chat;
    }
    if (update_.my_chat_member) {
        return &update_.my_chat_member->chat;
    }
    if (update_.chat_join_request) {
        return &update_.chat_join_request->chat;
    }
    return nullptr;
}

Message Context::reply(std::string text) const {
    const auto* target = chat();
    if (!target) {
        throw std::logic_error("this update has no reply chat");
    }
    const auto* source = message();
    if (!source) {
        source = callback_message(callback_query());
    }
    if (source && (source->guest_query_id || source->ephemeral_message_id)) {
        throw std::logic_error("use the explicit Bot API methods for guest or ephemeral replies");
    }
    SendMessageRequest request{};
    request.chat_id = target->id;
    request.text = std::move(text);
    if (source) {
        request.message_thread_id = source->message_thread_id;
        request.business_connection_id = source->business_connection_id;
        if (source->direct_messages_topic) {
            request.direct_messages_topic_id = source->direct_messages_topic->topic_id;
        }
        if (source->message_id > 0) {
            ReplyParameters parameters{};
            parameters.message_id = source->message_id;
            request.reply_parameters = std::move(parameters);
        }
    }
    return bot_.send_message(request);
}

bool Context::answer_callback(std::string text, bool alert) const {
    const auto* callback = callback_query();
    if (!callback) {
        throw std::logic_error("this update has no callback query");
    }
    AnswerCallbackQueryRequest request{};
    request.callback_query_id = callback->id;
    if (!text.empty()) {
        request.text = std::move(text);
    }
    request.show_alert = alert;
    return bot_.answer_callback_query(request);
}
} // namespace tgpp::ext
