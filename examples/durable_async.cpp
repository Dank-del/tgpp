#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/async_curl.hpp>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/sqlite_stores.hpp>

int main() {
    try {
        const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (!token) {
            throw std::runtime_error("Set TELEGRAM_BOT_TOKEN");
        }
        tgpp::Runtime runtime;
        auto store = std::make_shared<tgpp::ext::SqliteStores>("bot.sqlite");
        auto transport = std::make_shared<tgpp::AsyncCurlTransport>(token);
        tgpp::ext::AsyncApplication app(runtime.executor(), transport);
        // This identifier is a stable non-secret namespace, not the bot token.
        app.use(tgpp::ext::sessions(store, "example-bot"));
        app.add(
            "counter",
            [](const tgpp::ext::AsyncContext& c) { return c.message() && c.message()->text; },
            [](tgpp::ext::AsyncContext& context) -> tgpp::Task<void> {
                auto& session = context.get<tgpp::ext::SessionData>().value;
                auto count = session.value("count", 0) + 1;
                session["count"] = count;
                co_await context.reply("Message " + std::to_string(count));
            });
        tgpp::ext::AsyncPollingOptions options;
        options.store = store;
        options.bot_identity = "example-bot";
        options.owner = "worker-1";
        auto polling = std::make_shared<tgpp::ext::AsyncPolling>(app, options);
        auto stopping = std::make_shared<std::atomic<bool>>(false);
        asio::signal_set signals(runtime.executor(), SIGINT, SIGTERM);
        signals.async_wait([weak = std::weak_ptr(polling), stopping](auto error, int) {
            if (!error) {
                *stopping = true;
                if (auto active = weak.lock()) {
                    active->request_stop();
                }
            }
        });
        auto consume = [](std::shared_ptr<tgpp::ext::AsyncPolling> polling,
                          std::shared_ptr<std::atomic<bool>> stopping) -> tgpp::Task<void> {
            auto executor = co_await asio::this_coro::executor;
            while (!*stopping) {
                try {
                    if (co_await polling->process_pending()) {
                        continue;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "Update retained for retry: " << error.what() << '\n';
                }
                asio::steady_timer timer(executor, std::chrono::milliseconds(25));
                co_await timer.async_wait(asio::use_awaitable);
            }
        };
        auto worker =
            asio::co_spawn(runtime.executor(), consume(polling, stopping), asio::use_future);
        std::exception_ptr polling_error;
        try {
            asio::co_spawn(runtime.executor(), polling->run(), asio::use_future).get();
        } catch (...) {
            polling_error = std::current_exception();
        }
        *stopping = true;
        polling->request_stop();
        try {
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
        } catch (const tgpp::TimeoutError&) {
            auto shutdown_error = std::current_exception();
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
            if (!polling_error) {
                polling_error = shutdown_error;
            }
        }
        try {
            worker.get();
        } catch (...) {
            if (!polling_error) {
                polling_error = std::current_exception();
            }
        }
        if (polling_error) {
            std::rethrow_exception(polling_error);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
