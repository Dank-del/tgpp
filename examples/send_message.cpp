#include <tgpp/tgpp.hpp>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
    const auto* chat = std::getenv("TELEGRAM_TEST_CHAT_ID");
    if (!token || !chat || argc != 2) {
        std::cerr << "Set TELEGRAM_BOT_TOKEN and TELEGRAM_TEST_CHAT_ID; pass the message as one argument\n";
        return 1;
    }
    try {
        const std::string chat_value(chat);
        std::size_t parsed = 0;
        const auto chat_id = std::stoll(chat_value, &parsed);
        if (parsed != chat_value.size()) throw std::invalid_argument("chat ID must be an integer");
        tgpp::Bot bot(std::make_shared<tgpp::CurlTransport>(token));
        const auto identity = bot.get_me();
        tgpp::SendMessageRequest request{};
        request.chat_id = std::int64_t(chat_id);
        request.text = argv[1];
        const auto sent = bot.send_message(request);
        std::cout << "Sent message " << sent.message_id << " using @" << identity.username.value_or("") << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
