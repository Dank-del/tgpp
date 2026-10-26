#pragma once
#include "service.hpp"
#include <tgpp/ext/module.hpp>

namespace showcase {
class PrivateChat final : public tgpp::ext::Middleware {
public:
    tgpp::ext::Result handle(tgpp::ext::Context& context, tgpp::ext::Next next) override;
};

class ShowcaseModule final : public tgpp::ext::Module {
    std::shared_ptr<Sessions> sessions_;
public:
    explicit ShowcaseModule(std::shared_ptr<Sessions> sessions);
    std::string name() const override { return "showcase"; }
    void configure(tgpp::ext::Router& router) override;
    void start() override { sessions_->start(); }
    void stop() noexcept override { sessions_->stop(); }
};
} // namespace showcase
