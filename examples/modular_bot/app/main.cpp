#include "config.hpp"
#include "features/tasks/tasks_module.hpp"
#include "features/welcome/welcome_module.hpp"
#include "infrastructure/memory_task_repository.hpp"
#include "middleware/logging_middleware.hpp"
#include <csignal>
#include <iostream>
#include <tgpp/curl_transport.hpp>
#include <tgpp/ext/ext.hpp>

namespace {
volatile std::sig_atomic_t stopped = 0;

void stop(int) {
    stopped = 1;
}
} // namespace

int main() {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        const auto config = example::Config::from_environment();
        // This is the composition root: dependencies are created once here.
        auto repository = std::make_shared<example::MemoryTaskRepository>();
        auto tasks = std::make_shared<example::tasks::TaskService>(repository);
        auto transport = std::make_shared<tgpp::CurlTransport>(config.bot_token);
        tgpp::ext::Application app(transport);
        app.set_bot_username(app.bot().get_me().username.value_or(""));
        app.router().use<example::LoggingMiddleware>(std::clog);
        app.install<example::WelcomeModule>();
        app.install<example::tasks::TasksModule>(tasks);
        app.on_error([](tgpp::ext::Context& context, std::exception_ptr error) {
            std::cerr << "Handler failed at " << context.route() << '/' << context.handler_name()
                      << '\n';
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& cause) {
                std::cerr << cause.what() << '\n';
            }
            return tgpp::ext::ErrorAction::rethrow;
        });
        tgpp::ext::PollingOptions polling;
        polling.request.allowed_updates = std::vector<std::string>{"message"};
        tgpp::ext::Polling(app, polling).run([] { return !stopped; });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
