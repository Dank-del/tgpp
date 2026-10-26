#pragma once
#include <tgpp/ext/handler.hpp>

namespace example {
class StartCommand final : public tgpp::ext::CommandHandler {
  public:
    StartCommand() : CommandHandler("start") {}

    void handle(tgpp::ext::Context& context) override;
};
} // namespace example
