#include <tgpp/core.hpp>

namespace tgpp {
Json Client::call(const Request& request) const {
    const auto response = transport_->perform(request);
    if (!response.is_object() || !response.contains("ok") || !response.at("ok").is_boolean())
        throw std::runtime_error("invalid Telegram response envelope");
    if (!response.at("ok").get<bool>()) {
        const auto parameters = response.value("parameters", Json::object());
        std::optional<std::int64_t> retry, migrate;
        if (parameters.contains("retry_after")) retry = parameters.at("retry_after").get<std::int64_t>();
        if (parameters.contains("migrate_to_chat_id")) migrate = parameters.at("migrate_to_chat_id").get<std::int64_t>();
        throw ApiError(response.value("error_code", 0), response.value("description", "Telegram API error"), retry, migrate);
    }
    if (!response.contains("result")) throw std::runtime_error("Telegram response has no result");
    return response.at("result");
}

Json encode_file(const InputFile& file, Request* request) {
    if (!request) throw std::invalid_argument("serializing InputFile requires a Request for uploads");
    if (file.path.empty()) throw std::invalid_argument("upload path must not be empty");
    return request->attach(file);
}
} // namespace tgpp
