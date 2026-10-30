#pragma once
#include "../api_builders.hpp"
#include "../async_curl.hpp"
#include "fluent.hpp"
#include <concepts>
#include <type_traits>

namespace tgpp::ext {
namespace bot_app_detail {
template <class T> struct IsAwaitable : std::false_type {};

template <class T, class Executor>
struct IsAwaitable<asio::awaitable<T, Executor>> : std::true_type {};

template <class Function>
Task<void> invoke(std::shared_ptr<Function> function, AsyncContext& context) {
    using Result = std::invoke_result_t<Function&, AsyncContext&>;
    if constexpr (IsAwaitable<Result>::value) {
        co_await std::invoke(*function, context);
    } else {
        static_assert(std::is_void_v<Result>,
                      "BotApp handlers return void or an asio::awaitable result");
        std::invoke(*function, context);
    }
    co_return;
}

template <class Function> AsyncCallback adapt(Function function) {
    auto owned = std::make_shared<Function>(std::move(function));
    return [owned](AsyncContext& context) { return invoke(owned, context); };
}
} // namespace bot_app_detail

struct BotAppOptions {
    RuntimeOptions runtime;
    AsyncCurlOptions curl;
    AsyncApplicationOptions application;
    AsyncPollingOptions polling;
    std::size_t durable_workers = 4;
    std::chrono::milliseconds shutdown_grace{30000};
    bool handle_signals = true;
    bool bootstrap_identity = true;
};

// A complete polling bot. Register handlers, then run once on an external
// thread. stop() is safe from handlers, signal callbacks, and other threads.
class BotApp {
    struct State;
    std::shared_ptr<State> state_;
    void configuring() const;

  public:
    explicit BotApp(std::string token, BotAppOptions options = {});
    explicit BotApp(std::shared_ptr<AsyncTransport> transport, BotAppOptions options = {});
    ~BotApp();
    BotApp(const BotApp&) = delete;
    BotApp& operator=(const BotApp&) = delete;
    BotApp& command(std::string name, AsyncCallback callback);
    BotApp& command(std::string name, std::string reply_text);

    template <class Function>
    requires std::invocable<Function&, AsyncContext&> BotApp& command(std::string name,
                                                                      Function callback) {
        return command(std::move(name), bot_app_detail::adapt(std::move(callback)));
    }

    template <class Function>
    requires std::invocable<Function&, AsyncContext&> BotApp& on_text(Function callback) {
        return on_text(bot_app_detail::adapt(std::move(callback)));
    }

    template <class Function>
    requires std::invocable<Function&, AsyncContext&> BotApp& on_callback(std::string prefix,
                                                                          Function callback) {
        return on_callback(std::move(prefix), bot_app_detail::adapt(std::move(callback)));
    }

    BotApp& on_text(AsyncCallback callback);
    BotApp& on_callback(std::string prefix, AsyncCallback callback);
    BotApp& use(AsyncMiddleware middleware);
    BotApp& include(std::shared_ptr<AsyncRouter> router);
    BotApp& install(std::shared_ptr<AsyncModule> module);
    BotApp& on_error(AsyncErrorHandler handler);
    const AsyncBot& bot() const noexcept;
    ChatActions to(std::int64_t chat_id) const;
    ChatActions to(std::string chat_id) const;
    ApiActions api() const;
    // Escape hatches are initialized at construction; framework registration
    // freezes on first dispatch and BotApp registration closes when run starts.
    AsyncApplication& application() noexcept;
    Runtime& runtime() noexcept;
    AsyncPolling& polling() noexcept;
    void run();
    void stop() noexcept;
};
} // namespace tgpp::ext
