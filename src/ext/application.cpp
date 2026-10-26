#include <tgpp/ext/application.hpp>

namespace tgpp::ext {
Application::Application(std::shared_ptr<Transport> transport, ApplicationOptions options)
    : bot_(std::move(transport)), options_(std::move(options)) {}
Application::~Application() { stop(); }
void Application::ensure_configurable() const {
    if (state_ != State::configuring) throw std::logic_error("configure the application before starting it");
}
Router& Application::router() { ensure_configurable(); return router_; }
void Application::set_bot_username(std::string username) {
    ensure_configurable(); options_.bot_username = std::move(username);
}
void Application::on_error(ErrorHandler handler) {
    ensure_configurable(); error_handler_ = std::move(handler);
}
void Application::install(std::unique_ptr<Module> module) {
    ensure_configurable();
    if (!module) throw std::invalid_argument("module must not be null");
    const auto name = module->name();
    for (const auto& existing : modules_)
        if (existing->name() == name) throw std::invalid_argument("duplicate module: " + name);
    auto routes = std::make_unique<Router>(name);
    module->configure(*routes);
    router_.include(std::move(routes));
    modules_.push_back(std::move(module));
}
void Application::start() {
    if (state_ == State::started) return;
    ensure_configurable();
    router_.freeze();
    state_ = State::starting;
    try {
        while (started_modules_ < modules_.size()) {
            // Include the failing module in cleanup: stop() must tolerate a
            // partially completed start(), and is always noexcept.
            auto& module = modules_[started_modules_++];
            module->start();
            if (state_ != State::starting) throw std::logic_error("application stopped during startup");
        }
        state_ = State::started;
    } catch (...) {
        stop();
        throw;
    }
}
void Application::stop() noexcept {
    if (dispatching_) { stop_pending_ = true; return; }
    if (state_ == State::stopped) return;
    state_ = State::stopped;
    stop_pending_ = false;
    while (started_modules_) modules_[--started_modules_]->stop();
}
Result Application::process(const Update& update) {
    if (dispatching_) throw std::logic_error("recursive update dispatch is not supported");
    start();
    Context context(bot_, update, options_.bot_username);
    dispatching_ = true;
    struct Finish {
        bool& dispatching;
        bool& pending;
        Application& application;
        ~Finish() { dispatching = false; if (pending) application.stop(); }
    } finish{dispatching_, stop_pending_, *this};
    try { return router_.dispatch(context); }
    catch (...) {
        auto error = std::current_exception();
        if (error_handler_ && error_handler_(context, error) == ErrorAction::skip) return Result::failed;
        std::rethrow_exception(error);
    }
}
Result Application::process_json(const Json& update) { return process(from_json<Update>(update)); }
} // namespace tgpp::ext
