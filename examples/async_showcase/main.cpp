#include "async_showcase/service.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/async_curl.hpp>

int main() {
    try {
        const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (!token || !*token) {
            throw std::runtime_error("Set TELEGRAM_BOT_TOKEN");
        }
        tgpp::Runtime runtime;
        auto transport = std::make_shared<tgpp::AsyncCurlTransport>(token);
        tgpp::ext::AsyncApplication app(runtime.executor(), transport);
        const auto* assets = std::getenv("TGPP_SHOWCASE_ASSETS");
        const auto* reports = std::getenv("TGPP_ASYNC_SHOWCASE_REPORT_DIR");
        std::string username;
        auto bootstrap = [&]() -> tgpp::Task<void> {
            const auto identity = co_await app.bot().get_me();
            username = identity.username.value_or("");
            const auto webhook = co_await app.bot().get_webhook_info();
            if (!webhook.url.empty()) {
                throw std::runtime_error("Remove the existing webhook explicitly before polling");
            }
            std::cout << "Async showcase running as @" << identity.username.value_or("")
                      << "; 33 demos; /showcase opens the menu\n";
        };
        asio::co_spawn(runtime.executor(), bootstrap(), asio::use_future).get();
        auto sessions = std::make_shared<async_showcase::Sessions>(
            runtime.executor(),
            app.bot(),
            runtime.blocking_pool(),
            assets ? assets : TGPP_SHOWCASE_ASSETS,
            reports ? reports : "build/async_showcase_reports",
            username);

        app.add(
            "async-showcase",
            [](const tgpp::ext::AsyncContext& c) { return c.message() || c.callback_query(); },
            [sessions](tgpp::ext::AsyncContext& c) { return sessions->handle(c); });
        app.on_error([](tgpp::ext::AsyncContext&,
                        std::exception_ptr error) -> tgpp::Task<tgpp::ext::ErrorAction> {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& cause) {
                std::cerr << "Async showcase update: " << cause.what() << '\n';
            }
            co_return tgpp::ext::ErrorAction::skip;
        });
        tgpp::ext::AsyncPollingOptions options;
        options.polling.request.timeout = 15;
        options.polling.request.allowed_updates =
            std::vector<std::string>{"message", "callback_query"};
        auto polling = std::make_shared<tgpp::ext::AsyncPolling>(app, options);
        asio::signal_set signals(runtime.executor(), SIGINT, SIGTERM);
        signals.async_wait([weak = std::weak_ptr(polling)](auto error, int) {
            if (!error) {
                if (auto active = weak.lock()) {
                    active->request_stop();
                }
            }
        });
        std::exception_ptr failure;
        try {
            asio::co_spawn(runtime.executor(), polling->run(), asio::use_future).get();
        } catch (...) {
            failure = std::current_exception();
        }
        polling->request_stop();
        signals.cancel();
        bool drained = false;
        while (!drained) {
            try {
                asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
                drained = true;
            } catch (const tgpp::TimeoutError&) {
                // Cancellation was requested; join the resulting completions before
                // releasing the background service and its file pool dependencies.
                if (!failure) {
                    failure = std::current_exception();
                }
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
                break;
            }
        }
        asio::co_spawn(runtime.executor(), sessions->stop(), asio::use_future).get();
        signals.cancel();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
