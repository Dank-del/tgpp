#include "showcase/catalog.hpp"

namespace showcase {
using namespace tgpp;
using namespace std::chrono_literals;
void add_interactive_demos(Catalog& catalog) {
    catalog.add({"buttons", "Inline buttons", "interactive", [](DemoContext& ctx) {
        InlineKeyboardMarkup keyboard{};
        auto docs = InlineKeyboardButton{}; docs.text = "API docs ↗"; docs.url = "https://core.telegram.org/bots/api";
        auto copy = InlineKeyboardButton{}; copy.text = "Copy text"; CopyTextButton clipboard{}; clipboard.text = "Hello from tgpp!"; copy.copy_text = clipboard;
        keyboard.inline_keyboard = {{button("Count: 0", "show:count:0"), button("Show alert", "show:alert")}, {docs, copy}};
        SendMessageRequest request{}; request.chat_id = ctx.chat(); request.text = "tgpp · interactive inline keyboard\nThe counter edits this message. The alert acknowledges a callback query.";
        request.reply_markup = keyboard; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"keyboard", "Reply keyboard", "interactive", [](DemoContext& ctx) {
        ReplyKeyboardMarkup keyboard{};
        KeyboardButton menu{}; menu.text = "/showcase"; KeyboardButton report{}; report.text = "/report";
        keyboard.keyboard = {{menu, report}}; keyboard.resize_keyboard = true; keyboard.one_time_keyboard = true;
        keyboard.input_field_placeholder = "Try one of the command buttons";
        SendMessageRequest request{}; request.chat_id = ctx.chat(); request.text = "tgpp · reply keyboard\nCommand buttons are now below the input field. The tour removes this keyboard after four seconds.";
        request.reply_markup = keyboard; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
        ctx.pause(4s);
        ReplyKeyboardRemove remove{}; remove.remove_keyboard = true;
        request.text = "tgpp · reply keyboard removed"; request.reply_markup = remove;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"force_reply", "Force reply", "interactive", [](DemoContext& ctx) {
        ForceReply reply{}; reply.force_reply = true; reply.input_field_placeholder = "Write a reply to this demo";
        SendMessageRequest request{}; request.chat_id = ctx.chat(); request.text = "tgpp · reply to this message\nYour reply will be acknowledged by a separate handler class.";
        request.reply_markup = reply; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"poll", "Multiple-choice poll", "interactive", [](DemoContext& ctx) {
        SendPollRequest request{}; request.chat_id = ctx.chat(); request.question = "tgpp · which message types do you like?";
        for (const auto* text : {"Photos and albums", "Audio and video", "Buttons and polls"}) { InputPollOption option{}; option.text = text; request.options.push_back(option); }
        request.is_anonymous = true; request.allows_multiple_answers = true; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_poll(request); });
    }});
    catalog.add({"quiz", "Quiz", "interactive", [](DemoContext& ctx) {
        SendPollRequest request{}; request.chat_id = ctx.chat(); request.question = "tgpp · what does RAII stand for?";
        for (const auto* text : {"Resource Acquisition Is Initialization", "Really Awesome Inline Interfaces", "Runtime API Integration Interface"}) { InputPollOption option{}; option.text = text; request.options.push_back(option); }
        request.type = "quiz"; request.correct_option_ids = std::vector<std::int64_t>{0};
        request.explanation = "RAII ties a resource's lifetime to an object's lifetime."; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_poll(request); });
    }});
    catalog.add({"dice", "Dice + darts", "interactive", [](DemoContext& ctx) {
        for (const auto* emoji : {"🎲", "🎯"}) {
            SendDiceRequest request{}; request.chat_id = ctx.chat(); request.emoji = emoji; request.disable_notification = true;
            ctx.call([&] { return ctx.bot().send_dice(request); });
        }
    }});
    catalog.add({"typing", "Typing action", "interactive", [](DemoContext& ctx) {
        SendChatActionRequest request{}; request.chat_id = ctx.chat(); request.action = "typing";
        ctx.call([&] { return ctx.bot().send_chat_action(request); });
        ctx.pause(2s); ctx.text("tgpp · typing indicator finished\nThe app remained responsive while this worker prepared a message.");
    }});
}
} // namespace showcase
