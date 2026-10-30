// Deliberately keep this user-style fixture stable when updating the schema.
#include <concepts>
#include <tgpp/ext/fluent.hpp>
#include <utility>
using namespace tgpp;
using namespace tgpp::ext;
static_assert(std::same_as<decltype(std::declval<AsyncBot>()
                                        .to(std::int64_t(42))
                                        .text("hello")
                                        .html()
                                        .silent()
                                        .send()),
                           Task<Message>>);
static_assert(std::same_as<decltype(std::declval<AsyncBot>()
                                        .to(std::string("@channel"))
                                        .photo("file-id")
                                        .caption("hello")
                                        .send()),
                           Task<Message>>);
static_assert(
    std::same_as<decltype(std::declval<AsyncBot>().to(42).document(InputFile{"report.pdf"}).send()),
                 Task<Message>>);

Task<void> fluent_source_compatibility(AsyncBot bot, AsyncContext& context, AsyncApplication& app) {
    app.command("start", reply("Welcome!"));
    app.add("echo", async_filters::private_chat() & async_filters::text(), echo());
    auto buttons =
        keyboard({{callback_button("Next", "next"), url_button("Docs", "https://example.com")}});
    co_await context.reply("Hello");
    co_await context.say(html::bold("Hello <user>")).html().keyboard(buttons).send();
    co_await bot.to(42)
        .text("hello")
        .configure([](SendMessageRequest& request) {
            request.disable_notification = false;
            request.business_connection_id = "business";
            request.message_thread_id = 7;
        })
        .send();
    SendMessageRequest request{};
    request.chat_id = std::int64_t(42);
    request.text = "full generated API";
    co_await bot.send_message(std::move(request));
}
