#include <atomic>
#include <future>
#include <iostream>
#include <mutex>
#include <tgpp/ext/bot_app.hpp>

using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Mock final : AsyncTransport {
    std::mutex mutex;
    std::vector<Request> requests;
    std::string text = "/start@MockBot";
    bool command = true, fail_start = false, fail_poll = false;
    std::atomic<bool> starting = false;
    bool wait_start = false;
    int polls = 0;
    std::function<void()> on_send;

    Task<Json> perform(Request request) override {
        {
            std::lock_guard lock(mutex);
            requests.push_back(request);
        }
        if (request.method == "getMe") {
            starting = true;
            if (wait_start) {
                auto ex = co_await asio::this_coro::executor;
                while (!request.control->stop.stop_requested()) {
                    asio::steady_timer timer(ex, std::chrono::milliseconds(1));
                    co_await timer.async_wait(asio::use_awaitable);
                }
                throw CancelledError();
            }
            if (fail_start) {
                throw ApiError(401, "unauthorized");
            }
            co_return Json{
                {"ok", true},
                {"result",
                 {{"id", 123}, {"is_bot", true}, {"first_name", "test"}, {"username", "MockBot"}}}};
        }
        if (request.method == "getUpdates") {
            if (fail_poll) {
                throw ApiError(409, "conflict");
            }
            if (++polls == 1) {
                Json message = {{"message_id", 1},
                                {"date", 1},
                                {"chat", {{"id", 77}, {"type", "private"}}},
                                {"text", text}};
                if (command) {
                    message["entities"] = Json::array(
                        {{{"type", "bot_command"}, {"offset", 0}, {"length", text.size()}}});
                }
                co_return Json{{"ok", true},
                               {"result", Json::array({{{"update_id", 1}, {"message", message}}})}};
            }
            co_return Json{{"ok", true}, {"result", Json::array()}};
        }
        if (on_send) {
            on_send();
        }
        co_return Json{
            {"ok", true},
            {"result",
             {{"message_id", 42}, {"date", 1}, {"chat", {{"id", 77}, {"type", "private"}}}}}};
    }
};

BotAppOptions options() {
    BotAppOptions value;
    value.handle_signals = false;
    return value;
}

void run_with_deadline(BotApp& app) {
    auto result = std::async(std::launch::async, [&] { app.run(); });
    if (result.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        app.stop();
        result.wait();
        throw std::runtime_error("BotApp failed to stop");
    }
    result.get();
}

void sugar_and_identity() {
    auto mock = std::make_shared<Mock>();
    BotApp app(mock, options());
    std::string welcome = "Owned welcome";
    app.command("start", [welcome = std::move(welcome), &app](auto& context) {
        app.stop();
        return context.reply(welcome); // Task<Message> is adapted without boilerplate.
    });
    run_with_deadline(app);
    require(mock->polls == 1, "stop did not end polling");
    require(mock->requests.back().method == "sendMessage" &&
                mock->requests.back().parameters.at("text") == "Owned welcome",
            "owned Task<Message> callback failed");
    bool repeated = false;
    try {
        app.run();
    } catch (const std::logic_error&) {
        repeated = true;
    }
    require(repeated, "run accepted twice");
    bool frozen = false;
    try {
        app.command("late", "no");
    } catch (const std::logic_error&) {
        frozen = true;
    }
    require(frozen, "late handler accepted");
}

void string_reply() {
    auto mock = std::make_shared<Mock>();
    BotApp app(mock, options());
    mock->on_send = [&] { app.stop(); };
    app.command("start", "Welcome!");
    run_with_deadline(app);
    require(mock->requests.back().parameters.at("text") == "Welcome!",
            "string reply overload failed");
}

struct Module final : AsyncModule {
    int starts = 0, stops = 0;
    bool fail_stop = false;

    void configure(AsyncApplication&) override {}

    Task<void> start() override {
        ++starts;
        co_return;
    }

    Task<void> stop() override {
        ++stops;
        if (fail_stop) {
            throw std::runtime_error("stop failure");
        }
        co_return;
    }
};

void errors_and_cleanup() {
    auto mock = std::make_shared<Mock>();
    mock->command = false;
    mock->text = "plain";
    BotApp app(mock, options());
    auto module = std::make_shared<Module>();
    app.install(module);
    app.on_text([](auto&) { throw std::runtime_error("sync failure"); });
    bool failed = false;
    try {
        run_with_deadline(app);
    } catch (const std::runtime_error& error) {
        failed = std::string(error.what()) == "sync failure";
    }
    require(failed && module->starts == 1 && module->stops == 1,
            "handler error skipped module cleanup");
    auto startup = std::make_shared<Mock>();
    startup->fail_start = true;
    BotApp bad(startup, options());
    bool unauthorized = false;
    try {
        bad.run();
    } catch (const ApiError& error) {
        unauthorized = error.code == 401;
    }
    require(unauthorized && startup->polls == 0, "startup error lost");
    auto conflict = std::make_shared<Mock>();
    conflict->fail_poll = true;
    BotApp conflicting(conflict, options());
    bool conflicted = false;
    try {
        conflicting.run();
    } catch (const ApiError& error) {
        conflicted = error.code == 409;
    }
    require(conflicted, "polling error lost");
}

void cleanup_after_hook_failure() {
    auto mock = std::make_shared<Mock>();
    mock->command = false;
    mock->text = "plain";
    BotApp app(mock, options());
    auto first = std::make_shared<Module>();
    auto second = std::make_shared<Module>();
    second->fail_stop = true;
    app.install(first).install(second).on_text([&](auto&) { app.stop(); });
    bool failed = false;
    try {
        run_with_deadline(app);
    } catch (const std::runtime_error& error) {
        failed = std::string(error.what()) == "stop failure";
    }
    require(failed && first->stops == 1 && second->stops == 1,
            "failed cleanup skipped remaining modules");
}

void configuration_escape_hatches() {
    auto mock = std::make_shared<Mock>();
    auto configured = options();
    configured.application.bot_username = "ConfiguredBot";
    mock->text = "/start@ConfiguredBot";
    BotApp app(mock, configured);
    AsyncCallback raw = [&app](AsyncContext&) -> Task<void> {
        app.stop();
        co_return;
    };
    app.application().command("start", std::move(raw));
    auto prepared = app.to(77).text("prepared").html();
    auto future_api = app.api().get_me();
    (void)prepared;
    (void)future_api;
    require(mock->requests.empty(), "construction/registration/builders contacted network");
    run_with_deadline(app);
    auto no_bootstrap = std::make_shared<Mock>();
    no_bootstrap->text = "/start";
    auto advanced = options();
    advanced.bootstrap_identity = false;
    BotApp raw_app(no_bootstrap, advanced);
    raw_app.command("start", [&raw_app](auto&) { raw_app.stop(); });
    run_with_deadline(raw_app);
    require(!no_bootstrap->starting && no_bootstrap->requests.front().method == "getUpdates",
            "disabled bootstrap still contacted getMe");
}

void durable_parallelism_and_poll_fairness() {
    struct Empty final : AsyncTransport {
        Task<Json> perform(Request request) override {
            require(request.method == "getUpdates", "durable test unexpected method");
            co_return Json{{"ok", true}, {"result", Json::array()}};
        }
    };

    auto config = options();
    config.bootstrap_identity = false;
    config.runtime.executor_threads = 1;
    config.durable_workers = 4;
    config.shutdown_grace = std::chrono::milliseconds(20);
    auto store = std::make_shared<MemoryStores>();
    config.polling.store = store;
    config.polling.bot_identity = "parallel";
    BotApp app(std::make_shared<Empty>(), config);
    std::vector<InboxUpdate> updates;
    for (int id = 1; id <= 2; ++id) {
        Json message = {{"message_id", id},
                        {"date", 1},
                        {"chat", {{"id", id}, {"type", "private"}}},
                        {"text", "parallel"}};
        updates.push_back({id, std::to_string(id), Json{{"update_id", id}, {"message", message}}});
    }
    asio::co_spawn(
        app.runtime().executor(), store->enqueue("parallel", std::move(updates)), asio::use_future)
        .get();
    std::atomic<int> entered{0}, finished{0};
    app.on_text([&](AsyncContext&) -> Task<void> {
        ++entered;
        auto executor = co_await asio::this_coro::executor;
        // A cooperative barrier: neither handler may finish before both keys
        // have entered. One executor and immediate-empty polling prove fairness.
        while (entered != 2) {
            asio::steady_timer timer(executor, std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        if (++finished == 2) {
            app.stop();
        }
    });
    run_with_deadline(app);
    require(entered == 2 && finished == 2, "durable keys did not progress independently");
    auto invalid = options();
    invalid.durable_workers = 0;
    bool rejected = false;
    try {
        BotApp bad(std::make_shared<Empty>(), invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero durable workers accepted");
}

void startup_stop() {
    auto unused = std::make_shared<Mock>();
    BotApp stopped(unused, options());
    stopped.stop();
    stopped.run();
    require(unused->requests.empty(), "pre-stopped bot contacted network");
    auto waiting = std::make_shared<Mock>();
    waiting->wait_start = true;
    BotApp app(waiting, options());
    auto result = std::async(std::launch::async, [&] { app.run(); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!waiting->starting && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    app.stop();
    require(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
            "startup stop hung");
    result.get();
}
} // namespace

int main() {
    try {
        sugar_and_identity();
        string_reply();
        errors_and_cleanup();
        startup_stop();
        configuration_escape_hatches();
        cleanup_after_hook_failure();
        durable_parallelism_and_poll_fairness();
#ifndef TGPP_WITH_CURL
        bool explicit_error = false;
        try {
            BotApp disabled("123:token");
        } catch (const std::logic_error& error) {
            explicit_error = std::string(error.what()).find("AsyncTransport") != std::string::npos;
        }
        require(explicit_error, "curl-free constructor failure was not actionable");
#endif
        std::cout << "BotApp fluent lifecycle passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
