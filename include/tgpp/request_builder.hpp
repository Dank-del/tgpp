#pragma once
#include "async.hpp"

namespace tgpp {
class AsyncBot;
}

#include <bitset>
#include <concepts>
#include <type_traits>

namespace tgpp::api {
namespace detail {
template <class Request, class Result, auto Method, class Bot>
Task<Result> send_owned(Bot bot,
                        Request request,
                        std::vector<Attachment> attachments,
                        std::shared_ptr<RequestControl> control) {
    co_return co_await(bot.*Method)(std::move(request), std::move(attachments), std::move(control));
}
} // namespace detail

// Every schema-generated builder shares ownership and escape-hatch behavior.
// Named setters track required presence; accessing the raw mutable request or
// configure() is explicit opt-out, matching the low-level generated API.
template <class Request,
          class Result,
          auto Method,
          std::size_t Required,
          class Derived,
          class Bot = AsyncBot>
class ApiRequestBuilder {
  protected:
    Bot builder_bot_;
    Request builder_request_{};
    std::vector<Attachment> builder_attachments_;
    std::shared_ptr<RequestControl> builder_control_;
    std::bitset<Required> builder_required_;
    bool builder_raw_ = false;

    explicit ApiRequestBuilder(Bot bot) : builder_bot_(std::move(bot)) {}

    void mark_required(std::size_t index) {
        builder_required_.set(index);
    }

    Derived& self() {
        return static_cast<Derived&>(*this);
    }

  public:
    template <class F> Derived& configure(F&& configure) {
        std::invoke(std::forward<F>(configure), builder_request_);
        builder_raw_ = true;
        return self();
    }

    Derived& request(Request request) {
        builder_request_ = std::move(request);
        builder_raw_ = true;
        return self();
    }

    Request& request() noexcept {
        builder_raw_ = true;
        return builder_request_;
    }

    const Request& request() const noexcept {
        return builder_request_;
    }

    Derived& control(std::shared_ptr<RequestControl> control) {
        builder_control_ = std::move(control);
        return self();
    }

    Derived& attachments(std::vector<Attachment> attachments) {
        builder_attachments_ = std::move(attachments);
        return self();
    }

    Task<Result> send() const {
        if (!builder_raw_ && !builder_required_.all()) {
            throw std::invalid_argument("missing required Bot API fields; set all required fields "
                                        "or explicitly supply a complete raw request");
        }
        return detail::send_owned<Request, Result, Method>(
            builder_bot_, builder_request_, builder_attachments_, builder_control_);
    }
};
} // namespace tgpp::api
