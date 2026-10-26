#pragma once
#include "module.hpp"
#include <exception>

namespace tgpp::ext {
struct ApplicationOptions {
    // Needed to accept /command@your_bot. With no identity, addressed commands
    // are ignored. Plain /command works without an identity or getMe request.
    std::string bot_username;
};
enum class ErrorAction { rethrow, skip };
using ErrorHandler = std::function<ErrorAction(Context&, std::exception_ptr)>;

// A single-threaded dispatcher. Inject a transport for easy offline testing;
// long polling and webhook adapters share the same process() entry point.
class Application {
    enum class State { configuring, starting, started, stopped };
    Bot bot_;
    Router router_;
    ApplicationOptions options_;
    std::vector<std::unique_ptr<Module>> modules_;
    ErrorHandler error_handler_;
    State state_ = State::configuring;
    std::size_t started_modules_ = 0;
    bool dispatching_ = false;
    bool stop_pending_ = false;
    void ensure_configurable() const;
public:
    explicit Application(std::shared_ptr<Transport> transport, ApplicationOptions options = {});
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    const Bot& bot() const noexcept { return bot_; }
    bool is_running() const noexcept { return state_ == State::started; }
    Router& router();
    void set_bot_username(std::string username);
    void on_error(ErrorHandler handler);
    void install(std::unique_ptr<Module> module);
    template <class T, class... Args> T& install(Args&&... args) {
        auto value = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *value; install(std::move(value)); return result;
    }
    void start();
    void stop() noexcept;
    Result process(const Update& update);
    Result process_json(const Json& update);
};
} // namespace tgpp::ext
