#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/async_curl.hpp>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/webhook.hpp>

int main() {
    try {
        const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
        const auto* secret = std::getenv("TELEGRAM_WEBHOOK_SECRET");
        if (!token || !secret) {
            throw std::invalid_argument("Set TELEGRAM_BOT_TOKEN and TELEGRAM_WEBHOOK_SECRET");
        }
        tgpp::Runtime runtime;
        auto transport = std::make_shared<tgpp::AsyncCurlTransport>(token);
        tgpp::ext::AsyncApplication app(runtime.executor(), transport);
        app.command("start", [](tgpp::ext::AsyncContext& context) -> tgpp::Task<void> {
            co_await context.reply("Hello from the async webhook bot");
        });
        tgpp::ext::WebhookOptions options;
        options.path = "/telegram/updates";
        options.secret_token = secret;
        auto server = std::make_shared<tgpp::ext::WebhookServer>(runtime.executor(), options, app);
        asio::signal_set signals(runtime.executor(), SIGINT, SIGTERM);
        signals.async_wait([weak = std::weak_ptr(server)](std::error_code error, int) {
            if (!error) {
                if (auto live = weak.lock()) {
                    live->request_stop();
                }
            }
        });
        std::cout << "Webhook HTTP listener on 127.0.0.1:8080/telegram/updates; configure HTTPS "
                     "proxy and setWebhook separately\n";
        std::exception_ptr failure;
        try {
            asio::co_spawn(runtime.executor(), server->run(), asio::use_future).get();
        } catch (...) {
            failure = std::current_exception();
        }
        server->request_stop();
        signals.cancel();
        try {
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
        } catch (const tgpp::TimeoutError&) {
            // The first deadline requests cancellation; allow cooperative handlers to unwind.
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
        }
        transport->request_stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
