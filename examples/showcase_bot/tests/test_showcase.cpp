#include "showcase/catalog.hpp"
#include "showcase/module.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <tgpp/ext/application.hpp>

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Mock final : tgpp::Transport {
    std::vector<tgpp::Request> requests;
    std::int64_t next_id = 1;
    std::mutex mutex;
    std::condition_variable changed;

    tgpp::Json perform(const tgpp::Request& request) override {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(request);
        tgpp::Json result;
        static const std::set<std::string> booleans = {"sendChatAction",
                                                       "setMessageReaction",
                                                       "deleteMessage",
                                                       "sendMessageDraft",
                                                       "answerCallbackQuery"};
        if (booleans.count(request.method)) {
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
        changed.notify_all();
        return {{"ok", true}, {"result", result}};
    }

    bool wait_for_message(std::int64_t chat) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(4), [&] {
            return std::any_of(requests.begin(), requests.end(), [&](const auto& request) {
                return request.method == "sendMessage" && request.parameters.at("chat_id") == chat;
            });
        });
    }
};

void catalog_payloads() {
    auto transport = std::make_shared<Mock>();
    tgpp::Bot bot(transport);
    const auto catalog = showcase::make_catalog();
    require(catalog.demos().size() >= 30, "showcase should cover many message categories");
    showcase::DemoContext context(bot, 42, TGPP_SHOWCASE_ASSETS, [](auto) {});
    std::set<std::string> ids;
    for (const auto& demo : catalog.demos()) {
        require(ids.insert(demo.id).second && catalog.find(demo.id),
                "unique and addressable demos");
        demo.run(context);
    }
    std::set<std::string> methods;
    bool mixed_album = false, unicode_entities = false, reused = false, rich_blocks = false;
    for (const auto& request : transport->requests) {
        methods.insert(request.method);
        require(request.parameters.at("chat_id") == 42,
                "every demo must target its session's chat");
        for (const auto& upload : request.files) {
            require(std::filesystem::is_regular_file(upload.file.path),
                    "every upload fixture must exist");
        }
        if (request.method == "sendMediaGroup") {
            require(request.files.size() == 3, "album needs three named uploads");
            require(request.parameters.at("media").at(0).at("type") == "photo" &&
                        request.parameters.at("media").at(2).at("type") == "video",
                    "mixed album typed serialization");
            mixed_album = true;
        }
        if (request.method == "sendMessage" && request.parameters.contains("entities")) {
            require(request.parameters.at("entities").at(0).at("offset") == 3,
                    "astral emoji needs two UTF-16 units");
            unicode_entities = true;
        }
        if (request.method == "sendPhoto" && request.parameters.at("photo") == "reusable-file") {
            require(request.files.empty(), "reusing a file ID must not upload bytes");
            reused = true;
        }
        if (request.method == "sendRichMessage") {
            require(request.parameters.at("rich_message").at("blocks").at(0).at("type") ==
                        "heading",
                    "typed rich blocks");
            rich_blocks = true;
        }
        if (request.method == "sendPoll" &&
            request.parameters.value("type", std::string()) == "quiz") {
            require(request.parameters.at("correct_option_ids") == tgpp::Json::array({0}),
                    "latest quiz option representation");
        }
    }
    for (const auto* method : {"sendPhoto",
                               "sendVideo",
                               "sendAnimation",
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
        require(methods.count(method) != 0, "required message type was not exercised");
    }
    require(mixed_album && unicode_entities && reused && rich_blocks,
            "critical demo payload checks");
    for (const auto& row : catalog.keyboard().inline_keyboard) {
        for (const auto& button : row) {
            require(button.callback_data && button.callback_data->size() <= 64,
                    "menu callback data must fit Telegram's limit");
        }
    }
}

tgpp::Update callback(std::int64_t user, std::int64_t chat, std::string type, std::string data) {
    tgpp::Update update{};
    update.update_id = 1;
    tgpp::CallbackQuery query{};
    query.id = "query";
    query.from.id = user;
    query.data = std::move(data);
    tgpp::Message message{};
    message.message_id = 5;
    message.chat.id = chat;
    message.chat.type = std::move(type);
    tgpp::MaybeInaccessibleMessage parent{};
    parent.value = tgpp::Box<tgpp::Message>(message);
    query.message = parent;
    update.callback_query = query;
    return update;
}

void public_access_and_callbacks() {
    auto transport = std::make_shared<Mock>();
    tgpp::ext::Application app(transport);
    auto sessions = std::make_shared<showcase::Sessions>(
        app.bot(), TGPP_SHOWCASE_ASSETS, std::filesystem::path{});
    app.install<showcase::ShowcaseModule>(sessions);
    require(app.process(callback(7, 42, "private", "show:alert")) == tgpp::ext::Result::stopped,
            "a callback cannot claim another user's private chat");
    require(app.process(callback(42, 42, "group", "show:alert")) == tgpp::ext::Result::stopped,
            "group access must be blocked");
    require(transport->requests.empty(), "blocked access must not send messages");
    require(app.process(callback(7, 7, "private", "show:alert")) == tgpp::ext::Result::handled,
            "a different user must be able to use the bot");
    require(app.process(callback(42, 42, "private", "show:alert")) == tgpp::ext::Result::handled,
            "original user's callback routing");
    require(transport->requests.back().parameters.at("show_alert") == true,
            "alert should be a popup");
    app.process(callback(42, 42, "private", "show:count:0"));
    require(transport->requests.back().method == "editMessageText",
            "counter should edit the existing message");
    require(transport->requests.back()
                    .parameters.at("reply_markup")
                    .at("inline_keyboard")
                    .at(0)
                    .at(0)
                    .at("callback_data") == "show:count:1",
            "counter's next state");
    app.process(callback(42, 42, "private", "show:count:0"));
    require(transport->requests.back()
                    .parameters.at("reply_markup")
                    .at("inline_keyboard")
                    .at(0)
                    .at(0)
                    .at("callback_data") == "show:count:2",
            "rapid taps with stale callback data must still increment");
    app.process(callback(7, 7, "private", "show:count:0"));
    require(transport->requests.back().parameters.at("chat_id") == 7,
            "counter edits must target the caller's chat");
    require(transport->requests.back()
                    .parameters.at("reply_markup")
                    .at("inline_keyboard")
                    .at(0)
                    .at(0)
                    .at("callback_data") == "show:count:1",
            "identical message IDs in different chats must have independent counters");
    const auto before = transport->requests.size();
    app.process(callback(42, 42, "private", "show:count:garbage"));
    require(transport->requests.size() == before + 1 &&
                transport->requests.back().method == "answerCallbackQuery",
            "invalid counters must only be acknowledged");
    app.stop();
}

tgpp::Update command(std::int64_t chat, const std::string& text) {
    tgpp::Update update{};
    update.update_id = 1;
    tgpp::Message message{};
    message.message_id = 1;
    message.date = 1;
    message.chat.id = chat;
    message.chat.type = "private";
    tgpp::User user{};
    user.id = chat;
    message.from = user;
    message.text = text;
    tgpp::MessageEntity entity{};
    entity.type = "bot_command";
    entity.length = text.size();
    message.entities = std::vector<tgpp::MessageEntity>{entity};
    update.message = message;
    return update;
}

void independent_queues_and_cancellation() {
    auto transport = std::make_shared<Mock>();
    tgpp::ext::Application app(transport);
    auto sessions = std::make_shared<showcase::Sessions>(
        app.bot(), TGPP_SHOWCASE_ASSETS, std::filesystem::path{});
    app.install<showcase::ShowcaseModule>(sessions);
    require(app.process(command(42, "/start")) == tgpp::ext::Result::handled,
            "first user's start command");
    require(app.process(command(7, "/start")) == tgpp::ext::Result::handled,
            "second user's start command");
    require(sessions->get(42) != sessions->get(7), "different chats need independent services");
    sessions->get(42)->cancel();
    require(transport->wait_for_message(7),
            "cancelling one chat must not cancel another chat's menu");
    app.stop();

    // A busy session is not evicted; after stopping it, idle sessions can be
    // replaced so new users are not locked out by old, inactive conversations.
    tgpp::Bot bot(transport);
    showcase::Sessions bounded(bot, TGPP_SHOWCASE_ASSETS, {}, 1);
    bounded.start();
    const auto first = bounded.get(42);
    first->enqueue("tour");
    require(!bounded.get(7), "busy worker limit must be honored");
    bounded.stop();
    showcase::Sessions idle(bot, TGPP_SHOWCASE_ASSETS, {}, 1);
    idle.start();
    const auto old = idle.get(42);
    const auto next = idle.get(7);
    require(next && old != next && !old->enqueue("menu"),
            "idle eviction must admit a new chat without reusing old state");
    idle.stop();
}

void restored_report() {
    auto transport = std::make_shared<Mock>();
    tgpp::Bot bot(transport);
    const auto path =
        std::filesystem::temp_directory_path() /
        ("tgpp-showcase-report-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    const tgpp::Json saved = tgpp::Json::array(
        {{{"id", "photo"}, {"title", "Photo upload"}, {"group", "media"}, {"status", "passed"}}});
    {
        std::ofstream output(path);
        output << saved;
    }
    try {
        showcase::Service service(bot, 42, TGPP_SHOWCASE_ASSETS, path);
        require(service.report() == saved, "saved live reports must survive process restarts");
    } catch (...) {
        std::filesystem::remove(path);
        throw;
    }
    std::filesystem::remove(path);
}

void per_chat_reports() {
    auto transport = std::make_shared<Mock>();
    tgpp::Bot bot(transport);
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("tgpp-chat-reports-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto first =
        tgpp::Json::array({{{"id", "photo"}, {"title", "Photo upload"}, {"status", "passed"}}});
    const auto second =
        tgpp::Json::array({{{"id", "video"}, {"title", "Video"}, {"status", "failed"}}});
    {
        std::ofstream output(directory / "42.json");
        output << first;
    }
    {
        std::ofstream output(directory / "7.json");
        output << second;
    }
    try {
        showcase::Sessions sessions(bot, TGPP_SHOWCASE_ASSETS, directory);
        sessions.start();
        require(sessions.get(42)->report() == first, "first chat's persisted report");
        require(sessions.get(7)->report() == second,
                "second chat must not see the first chat's report");
        require(sessions.get(9)->report().empty(), "new chats must start with an empty report");
        sessions.stop();
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
    std::filesystem::remove_all(directory);
}
} // namespace

int main() {
    try {
        catalog_payloads();
        public_access_and_callbacks();
        independent_queues_and_cancellation();
        restored_report();
        per_chat_reports();
        std::cout << "Showcase payloads, public access, per-chat queues, cancellation, reports, "
                     "and callbacks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
