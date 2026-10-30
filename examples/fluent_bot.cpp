#include <cstdlib>
#include <iostream>
#include <tgpp/ext/bot_app.hpp>

int main() {
    try {
        const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
        if (!token) {
            throw std::runtime_error("Set TELEGRAM_BOT_TOKEN");
        }
        using namespace tgpp::ext;
        BotApp app(token);
        app.command("start", reply("Welcome!"));
        app.on_text(echo());
        app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
