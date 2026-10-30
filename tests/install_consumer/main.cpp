#include <atomic>
#include <iostream>
#include <tgpp/async_methods.hpp>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/bot_app.hpp>
#include <tgpp/ext/sqlite_stores.hpp>
#include <tgpp/ext/webhook.hpp>

class MockTransport final : public tgpp::AsyncTransport {
  public:
    std::atomic<int> calls{0};

    tgpp::Task<tgpp::Json> perform(tgpp::Request request) override {
        ++calls;
        if (request.method != "getMe") {
            throw std::runtime_error("unexpected request from installed consumer");
        }
        co_return tgpp::Json{{"ok", true},
                             {"result", {{"id", 123}, {"is_bot", true}, {"first_name", "test"}}}};
    }
};

int main() {
    tgpp::Runtime runtime({1, 1});
    auto transport = std::make_shared<MockTransport>();
    tgpp::AsyncBot bot(transport);
    auto identity = asio::co_spawn(runtime.executor(), bot.get_me(), asio::use_future).get();
    if (identity.id != 123) {
        return 1;
    }
    auto generated_identity =
        asio::co_spawn(runtime.executor(), bot.api().get_me().send(), asio::use_future).get();
    if (generated_identity.id != 123 || transport->calls != 2) {
        return 1;
    }
    tgpp::ext::BotAppOptions app_options;
    app_options.handle_signals = false;
    tgpp::ext::BotApp high_level(transport, app_options);
    high_level.command("start", "Welcome!");
    high_level.on_text([](tgpp::ext::AsyncContext& context) { return context.reply("Echo"); });
    auto configured = high_level.to(42).text("<b>Configured</b>").html().silent().send();
    auto factory = high_level.api().get_me();
    if (transport->calls != 2) {
        return 1;
    }
    auto high_identity =
        asio::co_spawn(high_level.runtime().executor(), factory.send(), asio::use_future).get();
    if (high_identity.id != 123) {
        return 1;
    }
    high_level.stop();
    high_level.run();
    if (transport->calls != 3) {
        return 1;
    }
    auto store = std::make_shared<tgpp::ext::SqliteStores>(":memory:");
    auto missing =
        asio::co_spawn(runtime.executor(), store->load({"consumer", "test"}), asio::use_future)
            .get();
    if (missing) {
        return 1;
    }
    tgpp::ext::AsyncApplication app(runtime.executor(), transport);
    tgpp::ext::WebhookOptions options;
    options.port = 0;
    options.path = "/updates";
    options.secret_token = "local-consumer-test";
    tgpp::ext::WebhookServer server(runtime.executor(), options, app);
    server.request_stop();
    asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
    std::cout << "installed SDK consumer passed\n";
}
