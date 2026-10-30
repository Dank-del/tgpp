#include <future>
#include <iostream>
#include <tgpp/ext/fluent.hpp>
using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Mock final : public AsyncTransport {
  public:
    std::vector<Request> requests;

    Task<Json> perform(Request request) override {
        requests.push_back(std::move(request));
        co_return Json{
            {"ok", true},
            {"result",
             {{"message_id", 9}, {"date", 1}, {"chat", {{"id", 42}, {"type", "private"}}}}}};
    }
};

void builders() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    auto markup = keyboard(
        {{callback_button("More", "show:more"), url_button("Docs", "https://example.com")}});
    auto control = std::make_shared<RequestControl>();
    SendMessageRequest raw{};
    raw.chat_id = std::int64_t(42);
    raw.text = "<b>Hello</b>";
    raw.parse_mode = "HTML";
    raw.disable_notification = true;
    raw.protect_content = true;
    raw.message_thread_id = 7;
    raw.business_connection_id = "business";
    raw.reply_markup = markup;
    ReplyParameters reply{};
    reply.message_id = 3;
    raw.reply_parameters = reply;
    auto task = bot.to(42)
                    .text("<b>Hello</b>")
                    .html()
                    .silent()
                    .protect_content()
                    .thread(7)
                    .business("business")
                    .reply_to(3)
                    .keyboard(markup)
                    .control(control)
                    .send();
    require(transport->requests.empty(), "configuration or task creation performed hidden I/O");
    // The complete temporary builder chain is gone before the coroutine starts.
    asio::co_spawn(runtime.executor(), std::move(task), asio::use_future).get();
    require(transport->requests.size() == 1, "terminal send made more than one API call");
    require(transport->requests.back().parameters == make_request(raw).parameters,
            "fluent text differs from generated request");
    require(transport->requests.back().control == control, "request control was not forwarded");
    auto owned_task = [&] {
        AsyncBot temporary(transport);
        std::string text = "original", chat = "@channel";
        auto result = temporary.to(chat).text(text).send();
        text = "changed";
        chat = "@changed";
        return result;
    }();
    asio::co_spawn(runtime.executor(), std::move(owned_task), asio::use_future).get();
    require(transport->requests.back().parameters["text"] == "original" &&
                transport->requests.back().parameters["chat_id"] == "@channel",
            "builder retained borrowed input/client");
    auto configured =
        bot.to(42)
            .text("configured")
            .silent(false)
            .configure([](SendMessageRequest& request) { request.message_effect_id = "effect"; })
            .send();
    asio::co_spawn(runtime.executor(), std::move(configured), asio::use_future).get();
    auto p = transport->requests.back().parameters;
    require(p["chat_id"] == 42 && p["text"] == "configured" && p["message_effect_id"] == "effect" &&
                p["disable_notification"] == false,
            "configure dropped defaults or explicit false");
    SendMessageRequest replacement{};
    replacement.chat_id = std::string("@replacement");
    replacement.text = "raw";
    auto replaced = bot.to(42).text("unused").html().request(replacement).send();
    asio::co_spawn(runtime.executor(), std::move(replaced), asio::use_future).get();
    require(transport->requests.back().parameters == make_request(replacement).parameters,
            "complete raw override failed");
    auto photo = bot.to(42).photo("file-id").caption("caption").markdown_v2().silent().send();
    asio::co_spawn(runtime.executor(), std::move(photo), asio::use_future).get();
    require(transport->requests.back().method == "sendPhoto" &&
                transport->requests.back().parameters["photo"] == "file-id",
            "photo helper failed");
    auto document = bot.to(42).document(InputFile{"sample.bin"}).caption("upload").send();
    asio::co_spawn(runtime.executor(), std::move(document), asio::use_future).get();
    require(transport->requests.back().method == "sendDocument" &&
                transport->requests.back().files.size() == 1 &&
                transport->requests.back().files[0].file.path == "sample.bin",
            "document upload helper failed");
}

void context_defaults_and_attachments() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    Update update{};
    update.update_id = 1;
    Message message{};
    message.message_id = 9;
    message.date = 1;
    message.chat.id = 42;
    message.chat.type = "private";
    message.message_thread_id = 7;
    message.business_connection_id = "business";
    DirectMessagesTopic topic{};
    topic.topic_id = 11;
    message.direct_messages_topic = topic;
    update.message = message;
    auto task = [&] {
        AsyncContext context(bot, update);
        return context.say("context")
            .html()
            .configure([](SendMessageRequest& request) {
                request.message_thread_id = 8;
                request.business_connection_id = "override";
                request.disable_notification = false;
                request.allow_paid_broadcast = false;
            })
            .send();
    }();
    asio::co_spawn(runtime.executor(), std::move(task), asio::use_future).get();
    SendMessageRequest direct{};
    direct.chat_id = std::int64_t(42);
    direct.text = "context";
    direct.parse_mode = "HTML";
    direct.message_thread_id = 8;
    direct.business_connection_id = "override";
    direct.direct_messages_topic_id = 11;
    direct.disable_notification = false;
    direct.allow_paid_broadcast = false;
    ReplyParameters reply{};
    reply.message_id = 9;
    direct.reply_parameters = reply;
    require(transport->requests.back().parameters == make_request(direct).parameters,
            "context defaults or configure overrides lost");
    auto photo = bot.to(42)
                     .photo("attach://image")
                     .attachments({{"image", InputFile{"image.png"}}})
                     .configure([](SendPhotoRequest& request) {
                         request.has_spoiler = true;
                         request.show_caption_above_media = false;
                     })
                     .send();
    asio::co_spawn(runtime.executor(), std::move(photo), asio::use_future).get();
    SendPhotoRequest raw{};
    raw.chat_id = std::int64_t(42);
    raw.photo = std::string("attach://image");
    raw.has_spoiler = true;
    raw.show_caption_above_media = false;
    auto expected = make_request(raw, {{"image", InputFile{"image.png"}}});
    auto& actual = transport->requests.back();
    require(actual.parameters == expected.parameters && actual.files.size() == 1 &&
                actual.files[0].name == "image" && actual.files[0].file.path == "image.png",
            "named attachments or full media options lost");
}

void markup_and_filters() {
    require(html::escape("<&\"'>") == "&lt;&amp;&quot;&#39;&gt;", "HTML escaping failed");
    require(html::bold("<name>") == "<b>&lt;name&gt;</b>", "bold escaped incorrectly");
    require(html::italic("a&b") == "<i>a&amp;b</i>" && html::code("<>") == "<code>&lt;&gt;</code>",
            "format helpers failed");
    require(html::link("<label>", "https://example.com?a=\"&b=2") ==
                "<a href=\"https://example.com?a=&quot;&amp;b=2\">&lt;label&gt;</a>",
            "link allowed markup injection");
    bool invalid = false;
    try {
        callback_button("Title", std::string(65, 'x'));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    require(invalid, "callback data byte limit not enforced");
    invalid = false;
    try {
        keyboard({{}});
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    require(invalid, "empty keyboard row accepted");
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    Update update{};
    update.update_id = 1;
    Message message{};
    message.chat.id = 42;
    message.chat.type = "private";
    message.message_id = 1;
    message.date = 1;
    message.text = "/start";
    MessageEntity entity{};
    entity.type = "bot_command";
    entity.offset = 0;
    entity.length = 6;
    message.entities = std::vector<MessageEntity>{entity};
    update.message = message;
    AsyncContext context(bot, update);
    auto filter = async_filters::text() & async_filters::private_chat() &
                  !async_filters::callback_prefix("unused");
    require(filter(context) &&
                (async_filters::command("start") | async_filters::callback_prefix("x"))(context),
            "composable async filters failed");
    Runtime runtime;
    AsyncApplication app(runtime.executor(), transport);
    app.command("start", reply("Welcome!"));
    asio::co_spawn(runtime.executor(), app.process(update), asio::use_future).get();
    require(transport->requests.back().parameters["text"] == "Welcome!",
            "reply callback helper failed");
}
} // namespace

int main() {
    try {
        builders();
        markup_and_filters();
        context_defaults_and_attachments();
        std::cout << "fluent tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
