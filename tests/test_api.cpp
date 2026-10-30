#include <iostream>
#include <limits>
#include <tgpp/tgpp.hpp>

using namespace tgpp;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Function> void rejects(Function function) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    require(threw, "expected operation to reject invalid input");
}

struct MockTransport : Transport {
    Request last;
    Json response;

    Json perform(const Request& request) override {
        last = request;
        return response;
    }
};

Json message_json() {
    return {{"message_id", 12},
            {"date", 1700000000},
            {"chat", {{"id", -1001234567890LL}, {"type", "supergroup"}}},
            {"text", "hello"}};
}

int main() {
    try {
        SendMessageRequest options{};
        options.chat_id = std::int64_t(-1001234567890LL);
        options.text = "Hello, 世界";
        auto request = make_request(options);
        require(request.method == "sendMessage", "incorrect method");
        require(!request.parameters.contains("disable_notification"), "unset field must be absent");
        options.disable_notification = false;
        request = make_request(options);
        require(request.parameters.at("disable_notification") == false,
                "explicit false must survive");
        options.chat_id = std::string("@channel");
        require(make_request(options).parameters.at("chat_id") == "@channel",
                "string union alternative lost");

        auto source = message_json();
        source["reply_to_message"] = message_json();
        source["future_unknown_field"] = 1;
        auto message = from_json<Message>(source);
        require(message.chat.id == -1001234567890LL, "chat ID truncated");
        require(message.reply_to_message && (*message.reply_to_message)->message_id == 12,
                "recursive decode failed");
        auto copy = message;
        (*copy.reply_to_message)->message_id = 99;
        require((*message.reply_to_message)->message_id == 12,
                "Box copy must not alias mutable data");
        require(to_json(message).at("reply_to_message").at("message_id") == 12,
                "recursive encode failed");
        auto again = from_json<Message>(to_json(message));
        require(again.text == "hello", "message roundtrip failed");
        rejects([] { from_json<Message>(Json{{"message_id", 1}}); });
        rejects([] { from_json<std::int64_t>(Json(2.5)); });
        rejects([] { from_json<bool>(Json(1)); });
        rejects([] { from_json<std::int64_t>(Json(std::numeric_limits<std::uint64_t>::max())); });
        require(from_json<std::vector<bool>>(Json::array({true, false})) ==
                    std::vector<bool>({true, false}),
                "boolean array decode failed");
        std::vector<std::int64_t> preserved{7};
        rejects([&] {
            Codec<std::vector<std::int64_t>>::decode(Json::array({1, "invalid"}), preserved);
        });
        require(preserved == std::vector<std::int64_t>({7}),
                "failed array decode mutated destination");
        require(from_json<std::int64_t>(Json(std::numeric_limits<std::int64_t>::max())) ==
                    std::numeric_limits<std::int64_t>::max(),
                "int64 range lost");

        auto reaction = from_json<ReactionType>(Json{{"type", "emoji"}, {"emoji", "👍"}});
        require(to_json(reaction).at("type") == "emoji", "tagged union decode failed");
        rejects([] { from_json<ReactionType>(Json{{"type", "future_variant"}, {"emoji", "x"}}); });
        auto inaccessible_json = message_json();
        inaccessible_json["date"] = 0;
        auto inaccessible = from_json<MaybeInaccessibleMessage>(inaccessible_json);
        require(std::holds_alternative<InaccessibleMessage>(inaccessible.value),
                "date=0 must select InaccessibleMessage");
        auto rich = from_json<RichText>(Json("plain text"));
        require(to_json(rich) == "plain text", "RichText string alternative failed");
        auto rich_array = from_json<RichText>(Json::array({"one", "two"}));
        require(to_json(rich_array) == Json::array({"one", "two"}),
                "recursive RichText array failed");
        auto content = from_json<InputMessageContent>(Json{
            {"latitude", 1.0}, {"longitude", 2.0}, {"title", "Cafe"}, {"address", "Main Street"}});
        require(std::holds_alternative<InputVenueMessageContent>(content.value),
                "structural union must prefer venue over location");
        InputMediaPhoto photo{};
        photo.media = "file-id";
        require(to_json(photo).at("type") == "photo", "discriminator default missing");
        photo.type = "video";
        rejects([&] { to_json(photo); });

        SendDocumentRequest upload{};
        upload.chat_id = std::int64_t(1);
        upload.document = InputFile{"/tmp/test.bin", "test.bin"};
        auto multipart = make_request(upload);
        require(multipart.files.size() == 1, "upload was not extracted");
        require(multipart.parameters.at("document") == "attach://upload_0",
                "upload reference missing");
        auto reserved = make_request(upload, {{"upload_1", InputFile{"existing.bin"}}});
        require(reserved.parameters.at("document") == "attach://upload_2",
                "attachment reference collided");
        rejects([&] { to_json(upload); });

        auto mock = std::make_shared<MockTransport>();
        Bot bot(mock);
        mock->response = {
            {"ok", true},
            {"result", {{"id", 1234567890123LL}, {"is_bot", true}, {"first_name", "test"}}}};
        require(bot.get_me().id == 1234567890123LL, "generated get_me failed");
        require(mock->last.method == "getMe" && mock->last.parameters.empty(),
                "getMe request invalid");
        mock->response = {{"ok", true}, {"result", message_json()}};
        require(bot.send_message(options).message_id == 12, "generated send_message failed");
        mock->response = {{"ok", true}, {"result", true}};
        EditMessageTextRequest edit{};
        edit.text = "edited";
        auto edited = bot.edit_message_text(edit);
        require(std::holds_alternative<bool>(edited) && std::get<bool>(edited),
                "multi-return Boolean failed");
        mock->response = {{"ok", true}, {"result", message_json()}};
        require(std::holds_alternative<Message>(bot.edit_message_text(edit)),
                "multi-return Message failed");
        mock->response = {
            {"ok", false},
            {"error_code", 429},
            {"description", "retry later"},
            {"parameters", {{"retry_after", 5}, {"migrate_to_chat_id", -1009876543210LL}}}};
        try {
            bot.get_me();
            throw std::logic_error("ApiError not thrown");
        } catch (const ApiError& error) {
            require(error.code == 429 && error.retry_after == 5 &&
                        error.migrate_to_chat_id == -1009876543210LL,
                    "API error metadata lost");
        }
        mock->response = {{"result", true}};
        rejects([&] { bot.get_me(); });
        mock->response = {{"ok", true}};
        rejects([&] { bot.get_me(); });
        std::cout << "API, codecs, recursive types, unions, uploads, and errors passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
