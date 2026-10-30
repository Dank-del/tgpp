#include <limits>
#include <tgpp/core.hpp>

namespace tgpp {
Json Client::call(const Request& request) const {
    return decode_response(transport_->perform(request));
}

Json decode_response(const Json& response) {
    if (!response.is_object() || !response.contains("ok") || !response.at("ok").is_boolean()) {
        throw ProtocolError("invalid Telegram response envelope");
    }
    if (!response.at("ok").get<bool>()) {
        const auto parameters = response.value("parameters", Json::object());
        if (!parameters.is_object() ||
            (response.contains("error_code") && !response.at("error_code").is_number_integer()) ||
            (response.contains("description") && !response.at("description").is_string())) {
            throw ProtocolError("invalid Telegram error envelope");
        }
        const auto integer_parameter = [](const Json& value) -> std::int64_t {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() >
                     static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
                throw ProtocolError("invalid Telegram error parameter");
            }
            return value.get<std::int64_t>();
        };
        std::optional<std::int64_t> retry, migrate;
        if (parameters.contains("retry_after")) {
            retry = integer_parameter(parameters.at("retry_after"));
            if (*retry < 0) {
                throw ProtocolError("negative Telegram retry delay");
            }
        }
        if (parameters.contains("migrate_to_chat_id")) {
            migrate = integer_parameter(parameters.at("migrate_to_chat_id"));
        }
        const auto code =
            response.contains("error_code") ? integer_parameter(response.at("error_code")) : 0;
        if (code < std::numeric_limits<int>::min() || code > std::numeric_limits<int>::max()) {
            throw ProtocolError("Telegram error code is outside integer range");
        }
        throw ApiError(static_cast<int>(code),
                       response.value("description", "Telegram API error"),
                       retry,
                       migrate);
    }
    if (!response.contains("result")) {
        throw ProtocolError("Telegram response has no result");
    }
    return response.at("result");
}

Json encode_file(const InputFile& file, Request* request) {
    if (!request) {
        throw std::invalid_argument("serializing InputFile requires a Request for uploads");
    }
    if (file.path.empty() && !file.memory && !file.source) {
        throw std::invalid_argument("upload path must not be empty");
    }
    return request->attach(file);
}
} // namespace tgpp
