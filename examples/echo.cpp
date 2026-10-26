#include <csignal>
#include <cstdlib>
#include <iostream>
#include <tgpp/tgpp.hpp>

namespace {
volatile std::sig_atomic_t stopped = 0;

void stop(int) {
    stopped = 1;
}
} // namespace

int main() {
    const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token) {
        std::cerr << "Set TELEGRAM_BOT_TOKEN\n";
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        tgpp::Bot bot(std::make_shared<tgpp::CurlTransport>(token));
        std::cout << "Connected as " << bot.get_me().first_name << '\n';
        tgpp::GetUpdatesRequest polling{};
        polling.timeout = 30;
        polling.allowed_updates = std::vector<std::string>{"message"};
        while (!stopped) {
            for (const auto& update : bot.get_updates(polling)) {
                if (update.message && update.message->text) {
                    tgpp::SendMessageRequest reply{};
                    reply.chat_id = update.message->chat.id;
                    reply.text = *update.message->text;
                    bot.send_message(reply);
                }
                polling.offset = update.update_id + 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
