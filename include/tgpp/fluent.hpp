#pragma once
#include "async_methods.hpp"
#include <string_view>

namespace tgpp {
namespace html {
inline std::string escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        default:
            result += c;
        }
    }
    return result;
}

inline std::string bold(std::string_view value) {
    return "<b>" + escape(value) + "</b>";
}

inline std::string italic(std::string_view value) {
    return "<i>" + escape(value) + "</i>";
}

inline std::string code(std::string_view value) {
    return "<code>" + escape(value) + "</code>";
}

inline std::string link(std::string_view label, std::string_view url) {
    return "<a href=\"" + escape(url) + "\">" + escape(label) + "</a>";
}
} // namespace html

inline InlineKeyboardButton callback_button(std::string text, std::string data) {
    if (text.empty() || data.empty() || data.size() > 64) {
        throw std::invalid_argument("callback button needs text and 1..64 bytes of callback data");
    }
    InlineKeyboardButton button{};
    button.text = std::move(text);
    button.callback_data = std::move(data);
    return button;
}

inline InlineKeyboardButton url_button(std::string text, std::string url) {
    if (text.empty() || url.empty()) {
        throw std::invalid_argument("URL button needs text and URL");
    }
    InlineKeyboardButton button{};
    button.text = std::move(text);
    button.url = std::move(url);
    return button;
}

inline InlineKeyboardMarkup keyboard(std::vector<std::vector<InlineKeyboardButton>> rows) {
    for (auto& row : rows) {
        if (row.empty()) {
            throw std::invalid_argument("keyboard rows must not be empty");
        }
    }
    InlineKeyboardMarkup result{};
    result.inline_keyboard = std::move(rows);
    return result;
}

namespace detail {
template <class Request, auto Method>
Task<Message> fluent_send(AsyncBot bot,
                          Request request,
                          std::vector<Attachment> attachments,
                          std::shared_ptr<RequestControl> control) {
    co_return co_await(bot.*Method)(std::move(request), std::move(attachments), std::move(control));
}
} // namespace detail

// Builders own their client and request values. send() creates an independently
// owned coroutine frame, so a temporary builder may be destroyed before awaiting.
template <class Options, class Derived> class RequestBuilder {
  protected:
    AsyncBot bot_;
    Options request_;
    std::vector<Attachment> attachments_;
    std::shared_ptr<RequestControl> control_;

    RequestBuilder(AsyncBot bot, Options request)
        : bot_(std::move(bot)), request_(std::move(request)) {}

    Derived& self() {
        return static_cast<Derived&>(*this);
    }

  public:
    Derived& silent(bool enabled = true) {
        request_.disable_notification = enabled;
        return self();
    }

    Derived& protect_content(bool enabled = true) {
        request_.protect_content = enabled;
        return self();
    }

    Derived& html() {
        request_.parse_mode = "HTML";
        return self();
    }

    Derived& markdown_v2() {
        request_.parse_mode = "MarkdownV2";
        return self();
    }

    Derived& keyboard(InlineKeyboardMarkup markup) {
        request_.reply_markup = std::move(markup);
        return self();
    }

    Derived& reply_to(std::int64_t message_id) {
        ReplyParameters reply{};
        reply.message_id = message_id;
        request_.reply_parameters = std::move(reply);
        return self();
    }

    Derived& thread(std::int64_t thread_id) {
        request_.message_thread_id = thread_id;
        return self();
    }

    Derived& business(std::string id) {
        request_.business_connection_id = std::move(id);
        return self();
    }

    Derived& control(std::shared_ptr<RequestControl> control) {
        control_ = std::move(control);
        return self();
    }

    Derived& attachments(std::vector<Attachment> files) {
        attachments_ = std::move(files);
        return self();
    }

    // Mutate generated options synchronously while keeping the builder's defaults.
    template <class F> Derived& configure(F&& configure) {
        std::invoke(std::forward<F>(configure), request_);
        return self();
    }

    // Escape hatch: replacement is the complete generated request, including chat_id.
    Derived& request(Options options) {
        request_ = std::move(options);
        return self();
    }

    Options& request() noexcept {
        return request_;
    }

    const Options& request() const noexcept {
        return request_;
    }
};

class MessageBuilder final : public RequestBuilder<SendMessageRequest, MessageBuilder> {
    friend class ChatActions;

    MessageBuilder(AsyncBot bot, SendMessageRequest request)
        : RequestBuilder(std::move(bot), std::move(request)) {}

  public:
    Task<Message> send() const {
        return detail::fluent_send<SendMessageRequest, &AsyncBot::send_message>(
            bot_, request_, attachments_, control_);
    }
};

template <class Options, auto Method>
class MediaBuilder final : public RequestBuilder<Options, MediaBuilder<Options, Method>> {
    friend class ChatActions;
    using Base = RequestBuilder<Options, MediaBuilder<Options, Method>>;

    MediaBuilder(AsyncBot bot, Options request) : Base(std::move(bot), std::move(request)) {}

  public:
    MediaBuilder& caption(std::string text) {
        this->request_.caption = std::move(text);
        return *this;
    }

    Task<Message> send() const {
        return detail::fluent_send<Options, Method>(
            this->bot_, this->request_, this->attachments_, this->control_);
    }
};

using PhotoBuilder = MediaBuilder<SendPhotoRequest, &AsyncBot::send_photo>;
using DocumentBuilder = MediaBuilder<SendDocumentRequest, &AsyncBot::send_document>;

class ChatActions {
    AsyncBot bot_;
    std::variant<std::int64_t, std::string> chat_;
    friend class AsyncBot;

    ChatActions(AsyncBot bot, std::variant<std::int64_t, std::string> chat)
        : bot_(std::move(bot)), chat_(std::move(chat)) {}

  public:
    MessageBuilder text(std::string text) const {
        SendMessageRequest request{};
        request.chat_id = chat_;
        request.text = std::move(text);
        return MessageBuilder(bot_, std::move(request));
    }

    PhotoBuilder photo(std::variant<InputFile, std::string> file) const {
        SendPhotoRequest request{};
        request.chat_id = chat_;
        request.photo = std::move(file);
        return PhotoBuilder(bot_, std::move(request));
    }

    DocumentBuilder document(std::variant<InputFile, std::string> file) const {
        SendDocumentRequest request{};
        request.chat_id = chat_;
        request.document = std::move(file);
        return DocumentBuilder(bot_, std::move(request));
    }
};

inline ChatActions AsyncBot::to(std::int64_t chat_id) const {
    return ChatActions(*this, chat_id);
}

inline ChatActions AsyncBot::to(std::string chat_id) const {
    return ChatActions(*this, std::move(chat_id));
}
} // namespace tgpp
