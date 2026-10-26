#include "showcase/module.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/curl_transport.hpp>
#include <tgpp/ext/polling.hpp>

namespace {
volatile std::sig_atomic_t stopped = 0;

void stop(int) {
    stopped = 1;
}

std::string environment(const char* key) {
    const auto* value = std::getenv(key);
    if (!value || !*value) {
        throw std::invalid_argument(std::string("Set ") + key);
    }
    return value;
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        bool tour = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--tour") {
                tour = true;
            } else {
                throw std::invalid_argument("Usage: showcase_bot [--tour]");
            }
        }
        const auto token = environment("TELEGRAM_BOT_TOKEN");
        std::optional<std::int64_t> startup_chat;
        if (const auto* value = std::getenv("TELEGRAM_TEST_CHAT_ID"); value && *value) {
            const std::string chat_value(value);
            std::size_t parsed = 0;
            const auto chat = std::stoll(chat_value, &parsed);
            if (parsed != chat_value.size() || chat <= 0) {
                throw std::invalid_argument(
                    "TELEGRAM_TEST_CHAT_ID must be a positive private chat ID");
            }
            startup_chat = chat;
        }
        if (tour && !startup_chat) {
            throw std::invalid_argument(
                "Set TELEGRAM_TEST_CHAT_ID for a startup tour, or use /tour in any private chat");
        }
        auto transport = std::make_shared<tgpp::CurlTransport>(token);
        tgpp::ext::Application app(transport);
        const auto identity = app.bot().get_me();
        if (!app.bot().get_webhook_info().url.empty()) {
            throw std::runtime_error(
                "This bot already has a webhook. Run this example with a polling bot.");
        }
        app.set_bot_username(identity.username.value_or(""));
        tgpp::BotCommandScope scope{};
        scope.value = tgpp::BotCommandScopeAllPrivateChats{};
        tgpp::SetMyCommandsRequest commands{};
        commands.scope = scope;
        for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
                 {"showcase", "Open the message-type demo menu"},
                 {"tour", "Run the full message tour"},
                 {"demo", "Run one demo by ID"},
                 {"report", "Show the demo results"},
                 {"stop", "Cancel the tour"}}) {
            tgpp::BotCommand command{};
            command.command = entry.first;
            command.description = entry.second;
            commands.commands.push_back(command);
        }
        app.bot().set_my_commands(commands);
        const auto* custom_assets = std::getenv("TGPP_SHOWCASE_ASSETS");
        const auto* custom_reports = std::getenv("TGPP_SHOWCASE_REPORT_DIR");
        const std::filesystem::path reports =
            custom_reports ? custom_reports : "build/showcase_reports";
        // Preserve the previous single-chat run while adopting per-chat storage.
        if (startup_chat) {
            const auto* legacy_custom = std::getenv("TGPP_SHOWCASE_REPORT");
            const std::filesystem::path legacy =
                legacy_custom ? legacy_custom : "build/showcase_report.json";
            const auto destination = reports / (std::to_string(*startup_chat) + ".json");
            if (std::filesystem::is_regular_file(legacy) && !std::filesystem::exists(destination)) {
                std::filesystem::create_directories(reports);
                std::filesystem::copy_file(legacy, destination);
            }
        }
        auto sessions = std::make_shared<showcase::Sessions>(
            app.bot(), custom_assets ? custom_assets : TGPP_SHOWCASE_ASSETS, reports);
        app.install<showcase::ShowcaseModule>(sessions);
        app.on_error([](tgpp::ext::Context& context, std::exception_ptr error) {
            std::cerr << "showcase update failed at " << context.route() << '/'
                      << context.handler_name() << ": ";
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& cause) {
                std::cerr << cause.what() << std::endl;
            }
            return tgpp::ext::ErrorAction::skip;
        });
        app.start();
        if (startup_chat) {
            const auto service = sessions->get(*startup_chat);
            service->enqueue("menu");
            if (tour) {
                service->enqueue("tour");
            }
        }
        std::cout << "Showcase running as @" << identity.username.value_or("") << "; "
                  << showcase::make_catalog().demos().size() << " demos; open to all private chats"
                  << std::endl;
        tgpp::ext::PollingOptions polling;
        polling.request.timeout = 15;
        polling.request.allowed_updates = std::vector<std::string>{"message", "callback_query"};
        tgpp::ext::Polling(app, polling).run([] { return !stopped; });
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
