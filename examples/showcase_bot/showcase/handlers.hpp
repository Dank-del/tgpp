#pragma once
#include "service.hpp"
#include <tgpp/ext/handler.hpp>
#include <map>

namespace showcase {
class ShowcaseCommand final : public tgpp::ext::CommandHandler {
    std::shared_ptr<Sessions> sessions_;
    std::string action_;
public:
    ShowcaseCommand(std::string command, std::string action, std::shared_ptr<Sessions> sessions);
    void handle(tgpp::ext::Context& context) override;
};
class ShowcaseCallback final : public tgpp::ext::CallbackQueryHandler {
    std::shared_ptr<Sessions> sessions_;
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> counters_;
public:
    explicit ShowcaseCallback(std::shared_ptr<Sessions> sessions);
    void handle(tgpp::ext::Context& context) override;
};
class ReplyDemoHandler final : public tgpp::ext::Handler {
public:
    ReplyDemoHandler() : Handler("showcase_reply") {}
    bool matches(const tgpp::ext::Context& context) const override;
    void handle(tgpp::ext::Context& context) override;
};
} // namespace showcase
