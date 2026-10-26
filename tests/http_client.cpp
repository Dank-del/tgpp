#include <future>
#include <iostream>
#include <tgpp/tgpp.hpp>

using namespace tgpp;

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("HTTP integration assertion failed");
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    try {
        CurlOptions config{};
        config.base_url = argv[1];
        config.max_connections = 2;
        Bot bot(std::make_shared<CurlTransport>("123:test_token", config));
        require(bot.get_me().id == 123);
        SendMessageRequest text{};
        text.chat_id = std::int64_t(42);
        text.text = "Hello, 世界";
        text.disable_notification = false;
        require(bot.send_message(text).message_id == 1);

        SendDocumentRequest document{};
        document.chat_id = std::int64_t(42);
        document.document = InputFile{argv[2], "report.bin", "application/octet-stream"};
        require(bot.send_document(document).message_id == 1);

        InputMediaPhoto photo{};
        photo.media = "attach://photo";
        InputMediaVideo video{};
        video.media = "attach://video";
        SendMediaGroupRequest album{};
        album.chat_id = std::int64_t(42);
        album.media = {photo, video};
        require(bot.send_media_group(album,
                                     {{"photo", InputFile{argv[2], "photo.bin"}},
                                      {"video", InputFile{argv[2], "video.bin"}}})
                    .size() == 2);

        GetChatRequest chat{};
        chat.chat_id = std::int64_t(42);
        try {
            bot.get_chat(chat);
            return 3;
        } catch (const ApiError& error) {
            require(error.code == 429 && error.retry_after == 7);
        }
        try {
            bot.call(Request{"badJson"});
            return 4;
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()).find("invalid JSON") != std::string::npos);
        }
        std::vector<std::future<User>> parallel;
        for (int i = 0; i < 8; ++i) {
            parallel.push_back(std::async(std::launch::async, [&] { return bot.get_me(); }));
        }
        for (auto& result : parallel) {
            require(result.get().id == 123);
        }
        std::cout << "Local HTTP JSON, multipart, nested attachments, and errors passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
