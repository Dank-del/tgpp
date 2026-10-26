#pragma once
#include <ostream>
#include <tgpp/ext/middleware.hpp>

namespace example {
class LoggingMiddleware final : public tgpp::ext::Middleware {
    std::ostream& output_;

  public:
    explicit LoggingMiddleware(std::ostream& output) : output_(output) {}

    tgpp::ext::Result handle(tgpp::ext::Context& context, tgpp::ext::Next next) override;
};
} // namespace example
