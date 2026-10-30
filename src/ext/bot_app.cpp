#include <csignal>
#include <mutex>
#include <tgpp/ext/bot_app.hpp>

namespace tgpp::ext {
struct BotApp::State : std::enable_shared_from_this<BotApp::State> {
    BotAppOptions options;
    Runtime runtime;
    std::shared_ptr<AsyncTransport> transport;
    AsyncApplication application;
    std::shared_ptr<AsyncPolling> polling;
    std::atomic<bool> used{false}, stopped{false};
    std::stop_source stop_source;
    std::mutex mutex;
    std::exception_ptr failure;

    State(std::shared_ptr<AsyncTransport> transport, BotAppOptions options)
        : options(std::move(options)), runtime(this->options.runtime),
          transport(std::move(transport)),
          application(runtime.executor(), this->transport, this->options.application),
          polling(std::make_shared<AsyncPolling>(application, this->options.polling)) {
        if (!this->options.durable_workers) {
            throw std::invalid_argument("durable worker count must be positive");
        }
        if (this->options.shutdown_grace.count() <= 0) {
            throw std::invalid_argument("shutdown grace must be positive");
        }
    }

    void stop() noexcept {
        stopped = true;
        stop_source.request_stop();
        polling->request_stop();
    }

    void record(std::exception_ptr error) {
        std::lock_guard lock(mutex);
        if (!failure) {
            failure = error;
        }
    }

    bool expected_cancel(std::exception_ptr error) const {
        if (!stopped) {
            return false;
        }
        try {
            std::rethrow_exception(error);
        } catch (const CancelledError&) {
            return true;
        } catch (const asio::system_error& cause) {
            return cause.code() == asio::error::operation_aborted;
        } catch (...) {
            return false;
        }
    }

    static Task<void> bootstrap(std::shared_ptr<State> state) {
        if (state->stopped) {
            co_return;
        }
        auto control = std::make_shared<RequestControl>();
        control->stop = state->stop_source.get_token();
        control->deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(state->options.curl.timeout_seconds);
        auto identity = co_await state->application.bot().get_me({}, {}, control);
        if (state->options.application.bot_username.empty()) {
            state->application.set_bot_username(identity.username.value_or(""));
        }
    }

    static Task<void> consume(std::shared_ptr<State> state) {
        auto executor = co_await asio::this_coro::executor;
        while (!state->stopped) {
            if (co_await state->polling->process_pending()) {
                continue;
            }
            asio::steady_timer timer(executor, std::chrono::milliseconds(25));
            co_await timer.async_wait(asio::use_awaitable);
        }
    }
};

namespace {
std::shared_ptr<AsyncTransport> default_transport(std::string token,
                                                  const AsyncCurlOptions& options) {
#ifdef TGPP_WITH_CURL
    return std::make_shared<AsyncCurlTransport>(std::move(token), options);
#else
    (void)token;
    (void)options;
    throw std::logic_error(
        "BotApp token constructor requires curl; supply an AsyncTransport instead");
#endif
}
} // namespace

BotApp::BotApp(std::string token, BotAppOptions options)
    : BotApp(default_transport(std::move(token), options.curl), options) {}

BotApp::BotApp(std::shared_ptr<AsyncTransport> transport, BotAppOptions options)
    : state_(std::make_shared<State>(std::move(transport), std::move(options))) {}

BotApp::~BotApp() {
    stop();
}

void BotApp::configuring() const {
    if (state_->used) {
        throw std::logic_error("BotApp registration is closed after run starts");
    }
}

BotApp& BotApp::command(std::string name, AsyncCallback callback) {
    configuring();
    state_->application.command(std::move(name), std::move(callback));
    return *this;
}

BotApp& BotApp::command(std::string name, std::string reply_text) {
    return command(std::move(name), reply(std::move(reply_text)));
}

BotApp& BotApp::on_text(AsyncCallback callback) {
    configuring();
    state_->application.add("text", async_filters::text(), std::move(callback));
    return *this;
}

BotApp& BotApp::on_callback(std::string prefix, AsyncCallback callback) {
    configuring();
    state_->application.add(
        "callback", async_filters::callback_prefix(std::move(prefix)), std::move(callback));
    return *this;
}

BotApp& BotApp::use(AsyncMiddleware middleware) {
    configuring();
    state_->application.use(std::move(middleware));
    return *this;
}

BotApp& BotApp::include(std::shared_ptr<AsyncRouter> router) {
    configuring();
    state_->application.include(std::move(router));
    return *this;
}

BotApp& BotApp::install(std::shared_ptr<AsyncModule> module) {
    configuring();
    state_->application.install(std::move(module));
    return *this;
}

BotApp& BotApp::on_error(AsyncErrorHandler handler) {
    configuring();
    state_->application.on_error(std::move(handler));
    return *this;
}

const AsyncBot& BotApp::bot() const noexcept {
    return state_->application.bot();
}

ChatActions BotApp::to(std::int64_t chat_id) const {
    return bot().to(chat_id);
}

ChatActions BotApp::to(std::string chat_id) const {
    return bot().to(std::move(chat_id));
}

ApiActions BotApp::api() const {
    return bot().api();
}

AsyncApplication& BotApp::application() noexcept {
    return state_->application;
}

Runtime& BotApp::runtime() noexcept {
    return state_->runtime;
}

AsyncPolling& BotApp::polling() noexcept {
    return *state_->polling;
}

void BotApp::stop() noexcept {
    state_->stop();
}

void BotApp::run() {
    auto state = state_;
    if (state->used.exchange(true)) {
        throw std::logic_error("BotApp::run may only be called once");
    }
    std::shared_ptr<asio::signal_set> signals;
    std::vector<std::future<void>> processors;
    try {
        if (state->options.handle_signals) {
            signals =
                std::make_shared<asio::signal_set>(state->runtime.executor(), SIGINT, SIGTERM);
            signals->async_wait([weak = std::weak_ptr(state)](asio::error_code error, int) {
                if (!error) {
                    if (auto active = weak.lock()) {
                        active->stop();
                    }
                }
            });
        }
        if (!state->stopped && state->options.bootstrap_identity) {
            asio::co_spawn(state->runtime.executor(), State::bootstrap(state), asio::use_future)
                .get();
        }
        if (!state->stopped && state->options.polling.store) {
            processors.reserve(state->options.durable_workers);
            for (std::size_t index = 0; index < state->options.durable_workers && !state->stopped;
                 ++index) {
                auto done = std::make_shared<std::promise<void>>();
                auto ready = done->get_future();
                asio::co_spawn(state->runtime.executor(),
                               State::consume(state),
                               [state, done](std::exception_ptr error) {
                                   if (error && !state->stopped) {
                                       state->record(error);
                                   }
                                   state->stop();
                                   done->set_value();
                               });
                processors.push_back(std::move(ready));
            }
        }
        if (!state->stopped) {
            asio::co_spawn(state->runtime.executor(), state->polling->run(), asio::use_future)
                .get();
        }
    } catch (...) {
        auto error = std::current_exception();
        if (!state->expected_cancel(error)) {
            state->record(error);
        }
    }
    state->stop();
    for (;;) {
        try {
            asio::co_spawn(state->runtime.executor(),
                           state->application.shutdown(state->options.shutdown_grace),
                           asio::use_future)
                .get();
            break;
        } catch (const TimeoutError&) {
            // The grace expiration cancelled accepted SDK work. Await cleanup
            // before destroying handlers, stores or application dependencies.
            state->record(std::current_exception());
        } catch (...) {
            // Module shutdown removes a started hook before awaiting it. Keep
            // draining remaining hooks even if one cleanup hook fails.
            state->record(std::current_exception());
        }
    }
    for (auto& processor : processors) {
        try {
            processor.get();
        } catch (...) {
            state->record(std::current_exception());
        }
    }
    if (signals) {
        asio::error_code error;
        signals->cancel(error);
    }
    // Future readiness can precede a completion handler's return on another
    // executor thread. Join the owned runtime before releasing run's State
    // owner, so a final callback can never destroy its own Runtime and self-join.
    state->runtime.stop();
    state->runtime.join();
    std::exception_ptr failure;
    {
        std::lock_guard lock(state->mutex);
        failure = state->failure;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}
} // namespace tgpp::ext
