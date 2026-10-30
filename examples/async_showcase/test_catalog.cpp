#include "async_showcase/catalog.hpp"
#include "async_showcase/service.hpp"
#include <fstream>
#include <iostream>
#include <set>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Mock final : tgpp::AsyncTransport {
    std::vector<tgpp::Request> requests;
    std::int64_t next_id = 1;

    tgpp::Task<tgpp::Json> perform(tgpp::Request request) override {
        requests.push_back(request);
        const std::set<std::string> boolean = {"sendChatAction",
                                               "setMessageReaction",
                                               "deleteMessage",
                                               "sendMessageDraft",
                                               "answerCallbackQuery"};
        tgpp::Json result;
        if (boolean.count(request.method)) {
            result = true;
        } else {
            result = {{"message_id", next_id++},
                      {"date", 1},
                      {"chat",
                       {{"id", request.parameters.value("chat_id", tgpp::Json(42))},
                        {"type", "private"}}}};
            if (request.method == "sendPhoto") {
                result["photo"] = tgpp::Json::array({{{"file_id", "reusable-file"},
                                                      {"file_unique_id", "unique-file"},
                                                      {"width", 768},
                                                      {"height", 512}}});
            }
            if (request.method == "sendMediaGroup") {
                result = tgpp::Json::array({result, result, result});
            }
        }
        co_return tgpp::Json{{"ok", true}, {"result", std::move(result)}};
    }
};

tgpp::Task<void> catalogue(std::shared_ptr<Mock> transport) {
    const auto catalog = async_showcase::make_catalog();
    require(catalog.demos().size() == 33, "all33 original demos preserved");
    tgpp::AsyncBot bot(transport);
    auto no_wait = [](std::chrono::milliseconds) -> tgpp::Task<void> { co_return; };
    async_showcase::AsyncDemoContext context(bot, 42, TGPP_SHOWCASE_ASSETS, no_wait);
    std::set<std::string> ids, methods;
    for (const auto& demo : catalog.demos()) {
        require(ids.insert(demo.id).second && catalog.find(demo.id), "unique addressable demo");
        co_await demo.run(context);
    }
    bool mixed_album = false, unicode_entities = false, reused = false, rich = false;
    for (const auto& request : transport->requests) {
        methods.insert(request.method);
        require(request.parameters.at("chat_id") == 42, "demo stays in requested chat");
        for (const auto& file : request.files) {
            require(std::filesystem::is_regular_file(file.file.path), "upload fixture exists");
        }
        if (request.method == "sendMediaGroup") {
            mixed_album = request.files.size() == 3 &&
                          request.parameters.at("media").at(2).at("type") == "video";
        }
        if (request.parameters.contains("entities")) {
            unicode_entities = request.parameters.at("entities").at(0).at("offset") == 3;
        }
        if (request.method == "sendPhoto" && request.parameters.at("photo") == "reusable-file") {
            reused = request.files.empty();
        }
        if (request.method == "sendRichMessage") {
            rich = request.parameters.at("rich_message").at("blocks").at(0).at("type") == "heading";
        }
    }
    for (const auto* method : {"sendPhoto",
                               "sendMediaGroup",
                               "sendVideo",
                               "sendAudio",
                               "sendVoice",
                               "sendVideoNote",
                               "sendDocument",
                               "sendSticker",
                               "sendPoll",
                               "sendDice",
                               "sendContact",
                               "sendVenue",
                               "sendLocation",
                               "editMessageText",
                               "setMessageReaction",
                               "deleteMessage",
                               "copyMessage",
                               "forwardMessage",
                               "editMessageLiveLocation",
                               "stopMessageLiveLocation",
                               "sendRichMessage",
                               "sendMessageDraft"}) {
        require(methods.count(method) > 0, "expected API method exercised");
    }
    require(mixed_album && unicode_entities && reused && rich, "critical typed payloads preserved");
    for (const auto& row : catalog.keyboard().inline_keyboard) {
        for (const auto& button : row) {
            require(button.callback_data && button.callback_data->size() <= 64,
                    "callback data limit");
        }
    }
}

tgpp::Task<void> service_limits(std::shared_ptr<Mock> transport, tgpp::BlockingPool& pool) {
    auto executor = co_await asio::this_coro::executor;
    tgpp::AsyncBot bot(transport);
    async_showcase::Sessions sessions(
        executor, bot, pool, TGPP_SHOWCASE_ASSETS, "build/async_showcase_contract_reports");
    std::exception_ptr failure;
    try {
        require(!co_await sessions.enqueue(42, "missing-demo"), "unknown job rejected");
        for (std::int64_t chat = 42; chat < 50; ++chat) {
            require(co_await sessions.enqueue(chat, "tour"), "eight active chat sessions accepted");
        }
        require(!co_await sessions.enqueue(50, "tour"), "ninth busy session rejected");
        require(co_await sessions.enqueue(42, "formatting"), "first queued job accepted");
        require(co_await sessions.enqueue(42, "entities"), "second queued job accepted");
        require(co_await sessions.enqueue(42, "preview"), "third queued job accepted");
        require(!co_await sessions.enqueue(42, "reaction"), "fourth queued job rejected");
        for (std::int64_t chat = 42; chat < 50; ++chat) {
            co_await sessions.cancel(chat);
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await sessions.stop();
    require(!co_await sessions.enqueue(42, "tour"), "stopped manager rejects work");
    if (failure) {
        std::rethrow_exception(failure);
    }
}

tgpp::Task<void> reports(std::shared_ptr<Mock> transport, tgpp::BlockingPool& pool) {
    auto executor = co_await asio::this_coro::executor;
    tgpp::AsyncBot bot(transport);
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("tgpp-async-report-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "77.json";
    auto pause = [executor]() -> tgpp::Task<void> {
        asio::steady_timer timer(executor, std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
    };
    async_showcase::Sessions first(executor, bot, pool, TGPP_SHOWCASE_ASSETS, directory);
    std::exception_ptr failure;
    try {
        require(co_await first.enqueue(77, "formatting"), "completed demo accepted");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline) {
            co_await pause();
        }
        require(std::filesystem::exists(path), "completed demo did not persist report");
        std::ifstream input(path);
        tgpp::Json report;
        input >> report;
        require(report.is_array() && report.size() == 1 && report[0]["id"] == "formatting" &&
                    report[0]["status"] == "passed",
                "persisted report lost result");
    } catch (...) {
        failure = std::current_exception();
    }
    co_await first.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
    async_showcase::Sessions second(executor, bot, pool, TGPP_SHOWCASE_ASSETS, directory);
    const auto previous = transport->requests.size();
    try {
        require(co_await second.enqueue(77, "report"), "saved report accepted");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (transport->requests.size() == previous &&
               std::chrono::steady_clock::now() < deadline) {
            co_await pause();
        }
        require(transport->requests.size() > previous &&
                    transport->requests.back().parameters.at("text").get<std::string>().find(
                        "HTML formatting") != std::string::npos,
                "report did not reload after session restart");
    } catch (...) {
        failure = std::current_exception();
    }
    co_await second.stop();
    if (failure) {
        std::filesystem::remove_all(directory);
        std::rethrow_exception(failure);
    }
    {
        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "interrupted write{";
    }
    async_showcase::Sessions recovered(executor, bot, pool, TGPP_SHOWCASE_ASSETS, directory);
    const auto before_recovery = transport->requests.size();
    try {
        require(co_await recovered.enqueue(77, "menu"), "corrupt report blocked admission");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (transport->requests.size() == before_recovery &&
               std::chrono::steady_clock::now() < deadline) {
            co_await pause();
        }
        require(transport->requests.size() > before_recovery, "corrupt report blocked menu");
        require(std::filesystem::exists(path.string() + ".corrupt"),
                "corrupt report not quarantined");
        require(co_await recovered.enqueue(77, "formatting"), "recovered demo rejected");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline) {
            co_await pause();
        }
        require(std::filesystem::exists(path), "recovered demo did not persist");
        std::ifstream input(path);
        tgpp::Json report;
        input >> report;
        require(report.is_array() && report.size() == 1 && report[0]["status"] == "passed",
                "recovered report not valid");
    } catch (...) {
        failure = std::current_exception();
    }
    co_await recovered.stop();
    std::filesystem::remove_all(directory);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
} // namespace

int main() {
    try {
        asio::io_context io;
        auto transport = std::make_shared<Mock>();
        auto done = asio::co_spawn(io, catalogue(transport), asio::use_future);
        io.run();
        done.get();
        io.restart();
        tgpp::BlockingPool pool(2, 16);
        auto service = asio::co_spawn(io, service_limits(transport, pool), asio::use_future);
        io.run();
        service.get();
        io.restart();
        auto reporting = asio::co_spawn(io, reports(transport, pool), asio::use_future);
        io.run();
        reporting.get();
        std::cout << "All33 coroutine showcase demos passed offline\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
