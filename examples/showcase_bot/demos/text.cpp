#include "showcase/catalog.hpp"

namespace showcase {
using namespace tgpp;
using namespace std::chrono_literals;
void add_text_demos(Catalog& catalog) {
    catalog.add({"formatting", "HTML formatting", "text", [](DemoContext& ctx) {
        SendMessageRequest request{}; request.chat_id = ctx.chat(); request.parse_mode = "HTML";
        request.text = "<b>tgpp · HTML formatting</b>\n\n<b>Bold</b> · <i>italic</i> · <u>underline</u> · <s>strikethrough</s>\n"
                       "<tg-spoiler>Tap to reveal this spoiler</tg-spoiler>\n<code>tgpp::Bot</code>\n"
                       "<pre><code class=\"language-cpp\">bot.send_message(request);</code></pre>"
                       "<blockquote>Small handlers, clear features, generated API types.</blockquote>\n"
                       "<a href=\"https://core.telegram.org/bots/api\">Telegram Bot API reference</a>";
        request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"entities", "Unicode entities", "text", [](DemoContext& ctx) {
        SendMessageRequest request{}; request.chat_id = ctx.chat();
        request.text = "🚀 tgpp uses explicit UTF-16 entity offsets. Unicode survives: 日本語 · বাংলা · café";
        MessageEntity bold{}; bold.type = "bold"; bold.offset = 3; bold.length = 4;
        request.entities = std::vector<MessageEntity>{bold}; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"preview", "Link preview", "text", [](DemoContext& ctx) {
        SendMessageRequest request{}; request.chat_id = ctx.chat();
        request.text = "tgpp · link preview\nhttps://core.telegram.org/bots";
        LinkPreviewOptions options{}; options.prefer_large_media = true; options.show_above_text = true;
        request.link_preview_options = options; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"reply", "Reply + quote", "text", [](DemoContext& ctx) {
        const auto original = ctx.text("tgpp · original message\nA small C++ library can still build complex bots.");
        SendMessageRequest request{}; request.chat_id = ctx.chat();
        request.text = "This message replies to a quoted part of the previous one.";
        ReplyParameters reply{}; reply.message_id = original.message_id; reply.quote = "complex bots";
        request.reply_parameters = reply; request.disable_notification = true;
        ctx.call([&] { return ctx.bot().send_message(request); });
    }});
    catalog.add({"edit", "Message editing", "text", [](DemoContext& ctx) {
        const auto original = ctx.text("tgpp · editing demo\nWorking… this message will change in two seconds.");
        ctx.pause(2s);
        EditMessageTextRequest edit{}; edit.chat_id = ctx.chat(); edit.message_id = original.message_id;
        edit.text = "tgpp · editing demo\n✅ Edited in place by editMessageText. No second message needed.";
        ctx.call([&] { return ctx.bot().edit_message_text(edit); });
    }});
    catalog.add({"reaction", "Reactions", "text", [](DemoContext& ctx) {
        const auto original = ctx.text("tgpp · reaction demo\nThe bot will add a 👍 reaction to this message.");
        ReactionTypeEmoji emoji{}; emoji.emoji = "👍"; ReactionType reaction{}; reaction.value = emoji;
        SetMessageReactionRequest request{}; request.chat_id = ctx.chat(); request.message_id = original.message_id;
        request.reaction = std::vector<ReactionType>{reaction}; request.is_big = true;
        ctx.call([&] { return ctx.bot().set_message_reaction(request); });
    }});
    catalog.add({"copy", "Copy + forward", "text", [](DemoContext& ctx) {
        const auto original = ctx.text("tgpp · original for copy and forward\nThe next two messages demonstrate the difference.");
        CopyMessageRequest copy{}; copy.chat_id = ctx.chat(); copy.from_chat_id = ctx.chat(); copy.message_id = original.message_id;
        copy.disable_notification = true;
        ctx.call([&] { return ctx.bot().copy_message(copy); });
        ForwardMessageRequest forward{}; forward.chat_id = ctx.chat(); forward.from_chat_id = ctx.chat(); forward.message_id = original.message_id;
        forward.disable_notification = true;
        ctx.call([&] { return ctx.bot().forward_message(forward); });
    }});
    catalog.add({"delete", "Auto-delete demo", "text", [](DemoContext& ctx) {
        const auto original = ctx.text("tgpp · temporary message\nWatch this message disappear in three seconds.");
        ctx.pause(3s);
        DeleteMessageRequest request{}; request.chat_id = ctx.chat(); request.message_id = original.message_id;
        ctx.call([&] { return ctx.bot().delete_message(request); });
    }});
}
} // namespace showcase
