#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/async_curl.hpp>
#include <tgpp/ext/async.hpp>

int main() {
    try {
        const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (!token) {
            throw std::runtime_error("Set TELEGRAM_BOT_TOKEN");
        }
        tgpp::Runtime runtime;
        auto transport = std::make_shared<tgpp::AsyncCurlTransport>(token);
        tgpp::ext::AsyncApplication app(runtime.executor(), transport);
        app.add(
            "echo",
            [](const tgpp::ext::AsyncContext& c) { return c.message() && c.message()->text; },
            [](tgpp::ext::AsyncContext& c) -> tgpp::Task<void> {
                co_await c.reply(*c.message()->text);
            });
        auto polling = std::make_shared<tgpp::ext::AsyncPolling>(app);
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
        try {
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
        } catch (const tgpp::TimeoutError&) {
            // shutdown has cancelled SDK work after its grace period. Await the
            // resulting completions before releasing handler dependencies.
            asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
