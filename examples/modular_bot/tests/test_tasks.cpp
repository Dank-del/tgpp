#include "features/tasks/tasks_module.hpp"
#include "features/welcome/welcome_module.hpp"
#include "infrastructure/memory_task_repository.hpp"
#include <iostream>
#include <tgpp/ext/application.hpp>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct MockTransport final : tgpp::Transport {
    std::vector<tgpp::Request> requests;

    tgpp::Json perform(const tgpp::Request& request) override {
        requests.push_back(request);
        return {{"ok", true},
                {"result",
                 {{"message_id", 2}, {"date", 1}, {"chat", {{"id", 42}, {"type", "private"}}}}}};
    }
};

tgpp::Json command(std::string text, std::int64_t user = 7, std::string chat_type = "private") {
    const auto end = text.find(' ');
    return {{"update_id", 1},
            {"message",
             {{"message_id", 1},
              {"date", 1},
              {"chat", {{"id", 42}, {"type", chat_type}}},
              {"from", {{"id", user}, {"is_bot", false}, {"first_name", "Alice"}}},
              {"text", text},
              {"entities",
               tgpp::Json::array({{{"type", "bot_command"},
                                   {"offset", 0},
                                   {"length", end == std::string::npos ? text.size() : end}}})}}}};
}
} // namespace

int main() {
    try {
        auto repository = std::make_shared<example::MemoryTaskRepository>();
        auto service = std::make_shared<example::tasks::TaskService>(repository);
        require(service->add(99, "  buy milk  ").title == "buy milk",
                "service validation and normalization");
        require(service->list(7).empty(), "repository must isolate users");
        bool rejected = false;
        try {
            service->add(7, "  ");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && service->list(7).empty(), "invalid input must not be persisted");

        auto mock = std::make_shared<MockTransport>();
        tgpp::ext::Application app(mock, {"example_bot"});
        app.install<example::WelcomeModule>();
        app.install<example::tasks::TasksModule>(service);
        require(app.process_json(command("/start")) == tgpp::ext::Result::handled,
                "welcome module registration");
        require(app.process_json(command("/add@Example_Bot buy milk")) ==
                    tgpp::ext::Result::handled,
                "task command routing");
        require(service->list(7).size() == 1 && service->list(7)[0].title == "buy milk",
                "handler must call the injected service");
        app.process_json(command("/tasks"));
        require(mock->requests.back().parameters.at("text").get<std::string>().find("buy milk") !=
                    std::string::npos,
                "list handler must share injected state");
        app.process_json(command("/tasks", 8));
        require(mock->requests.back().parameters.at("text").get<std::string>().find(
                    "No tasks yet") != std::string::npos,
                "user isolation through real routing");
        app.process_json(command("/add"));
        require(service->list(7).size() == 1, "invalid command must not create a task");
        const auto before = mock->requests.size();
        require(app.process_json(command("/add group task", 7, "supergroup")) ==
                    tgpp::ext::Result::unhandled,
                "feature router privacy filter");
        require(mock->requests.size() == before && service->list(7).size() == 1,
                "group updates must not reach private task handlers");
        std::cout
            << "Modular bot domain, dependency injection, routes, and user isolation passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
