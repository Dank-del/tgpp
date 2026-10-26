#include <tgpp/tgpp.hpp>
#include <tgpp/ext/ext.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>

namespace { volatile std::sig_atomic_t stopped = 0; void stop(int) { stopped = 1; } }
int main() {
    const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token) { std::cerr << "Set TELEGRAM_BOT_TOKEN\n"; return 1; }
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        auto transport = std::make_shared<tgpp::CurlTransport>(token);
        tgpp::Bot bot(transport);
        const auto username = bot.get_me().username.value_or("");
        tgpp::ext::ConcurrentApplication app([transport, username] {
            auto worker = std::make_unique<tgpp::ext::Application>(transport, tgpp::ext::ApplicationOptions{username});
            worker->router().add<tgpp::ext::FunctionHandler>("ping", tgpp::ext::filters::command("ping"),
                [](tgpp::ext::Context& context) { context.reply("Pong — independent chats run concurrently; each chat stays ordered."); });
            return worker;
        }, {4, 256, {}});
        tgpp::ext::Polling(app, bot).run([] { return !stopped; });
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
