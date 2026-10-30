#include <future>
#include <iostream>
#include <tgpp/api_builders.hpp>
using namespace tgpp;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Mock final : public AsyncTransport {
  public:
    std::vector<Request> requests;

    Task<Json> perform(Request request) override {
        auto method = request.method;
        requests.push_back(std::move(request));
        if (method == "getMe") {
            co_return Json{{"ok", true},
                           {"result", {{"id", 1}, {"is_bot", true}, {"first_name", "Bot"}}}};
        }
        if (method == "getUpdates") {
            co_return Json{{"ok", true}, {"result", Json::array()}};
        }
        if (method == "deleteWebhook") {
            co_return Json{{"ok", true}, {"result", true}};
        }
        co_return Json{
            {"ok", true},
            {"result",
             {{"message_id", 9}, {"date", 1}, {"chat", {{"id", 42}, {"type", "private"}}}}}};
    }
};

void fields_and_ownership() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    auto control = std::make_shared<RequestControl>();
    SendMessageRequest raw{};
    raw.chat_id = std::int64_t(42);
    raw.text = "Hello";
    raw.parse_mode = "HTML";
    raw.disable_notification = false;
    raw.protect_content = true;
    raw.business_connection_id = "business";
    raw.message_thread_id = 8;
    auto task = bot.api()
                    .send_message()
                    .chat_id(42)
                    .text("Hello")
                    .parse_mode("HTML")
                    .disable_notification(false)
                    .protect_content(true)
                    .business_connection_id("business")
                    .message_thread_id(8)
                    .control(control)
                    .send();
    require(transport->requests.empty(), "builder performed I/O before awaiting");
    asio::co_spawn(runtime.executor(), std::move(task), asio::use_future).get();
    require(transport->requests.size() == 1 &&
                transport->requests.back().parameters == make_request(raw).parameters,
            "generated builder payload differs from raw request");
    require(transport->requests.back().control == control, "control lost");
    auto owned = [&] {
        AsyncBot temporary(transport);
        std::string chat = "@channel", text = "owned";
        auto result = temporary.api().send_message().chat_id(chat).text(text).send();
        chat.clear();
        text.clear();
        return result;
    }();
    asio::co_spawn(runtime.executor(), std::move(owned), asio::use_future).get();
    require(transport->requests.back().parameters["chat_id"] == "@channel" &&
                transport->requests.back().parameters["text"] == "owned",
            "generated builder borrowed input/client");
    auto reset = bot.api()
                     .send_message()
                     .chat_id(42)
                     .text("reset")
                     .parse_mode("HTML")
                     .parse_mode(std::nullopt)
                     .send();
    asio::co_spawn(runtime.executor(), std::move(reset), asio::use_future).get();
    require(!transport->requests.back().parameters.contains("parse_mode"),
            "optional setter could not reset field");
    auto zero_required = bot.api().get_me().send();
    require(
        asio::co_spawn(runtime.executor(), std::move(zero_required), asio::use_future).get().is_bot,
        "zero-required builder failed");
    require(asio::co_spawn(runtime.executor(),
                           bot.api().delete_webhook().drop_pending_updates(false).send(),
                           asio::use_future)
                .get(),
            "bool-result builder failed");
    require(transport->requests.back().parameters["drop_pending_updates"] == false,
            "explicit optional false lost");
}

void explicit_empty_optional_values() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    auto empty_array = bot.api().get_updates().allowed_updates({}).timeout(0).send();
    asio::co_spawn(runtime.executor(), std::move(empty_array), asio::use_future).get();
    auto p = transport->requests.back().parameters;
    require(p.contains("allowed_updates") && p["allowed_updates"].is_array() &&
                p["allowed_updates"].empty() && p["timeout"] == 0,
            "empty braces omitted optional array or zero");
    auto empty_caption = bot.api()
                             .send_photo()
                             .chat_id(42)
                             .photo("file-id")
                             .caption({})
                             .disable_notification(false)
                             .send();
    asio::co_spawn(runtime.executor(), std::move(empty_caption), asio::use_future).get();
    p = transport->requests.back().parameters;
    require(p.contains("caption") && p["caption"] == "" && p["disable_notification"] == false,
            "empty braces omitted optional string or false");
    std::optional<std::vector<std::string>> values(std::in_place);
    auto explicit_optional = bot.api().get_updates().allowed_updates(values).send();
    asio::co_spawn(runtime.executor(), std::move(explicit_optional), asio::use_future).get();
    require(transport->requests.back().parameters["allowed_updates"].is_array(),
            "explicit engaged optional was not accepted");
    auto cleared = bot.api().get_updates().allowed_updates({}).allowed_updates(std::nullopt).send();
    asio::co_spawn(runtime.executor(), std::move(cleared), asio::use_future).get();
    require(!transport->requests.back().parameters.contains("allowed_updates"),
            "nullopt failed to clear optional field");
    values.reset();
    auto optional_reset =
        bot.api().get_updates().allowed_updates({}).allowed_updates(values).send();
    asio::co_spawn(runtime.executor(), std::move(optional_reset), asio::use_future).get();
    require(!transport->requests.back().parameters.contains("allowed_updates"),
            "empty explicit optional failed to clear field");
}

void required_and_escape_hatches() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncBot bot(transport);
    auto before = transport->requests.size();
    bool missing = false;
    try {
        auto task = bot.api().send_message().chat_id(42).send();
        (void)task;
    } catch (const std::invalid_argument&) {
        missing = true;
    }
    require(missing && transport->requests.size() == before,
            "missing required setter was not rejected before I/O");
    SendMessageRequest raw{};
    raw.chat_id = std::string("@raw");
    raw.text = "raw";
    raw.link_preview_options = LinkPreviewOptions{};
    raw.link_preview_options->is_disabled = false;
    auto full = bot.api().send_message().request(raw).send();
    asio::co_spawn(runtime.executor(), std::move(full), asio::use_future).get();
    require(transport->requests.back().parameters == make_request(raw).parameters,
            "full raw request escape differs");
    auto builder = bot.api().send_message();
    builder.request() = raw;
    auto configured =
        builder.configure([](SendMessageRequest& request) { request.message_effect_id = "effect"; })
            .send();
    asio::co_spawn(runtime.executor(), std::move(configured), asio::use_future).get();
    raw.message_effect_id = "effect";
    require(transport->requests.back().parameters == make_request(raw).parameters,
            "configure/mutable request escape differs");
    SendPhotoRequest photo{};
    photo.chat_id = std::int64_t(42);
    photo.photo = std::string("attach://named");
    photo.has_spoiler = false;
    auto expected = make_request(photo, {{"named", InputFile{"image.png"}}});
    auto upload = bot.api()
                      .send_photo()
                      .chat_id(42)
                      .photo("attach://named")
                      .has_spoiler(false)
                      .attachments({{"named", InputFile{"image.png"}}})
                      .send();
    asio::co_spawn(runtime.executor(), std::move(upload), asio::use_future).get();
    auto& actual = transport->requests.back();
    require(actual.parameters == expected.parameters && actual.files.size() == 1 &&
                actual.files[0].name == "named",
            "generated media builder lost upload configuration");
}
} // namespace

int main() {
    try {
        fields_and_ownership();
        required_and_escape_hatches();
        explicit_empty_optional_values();
        std::cout << "API builder tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
