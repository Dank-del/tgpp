#pragma once
#include <tgpp/ext/module.hpp>

namespace example {
class WelcomeModule final : public tgpp::ext::Module {
public:
    std::string name() const override { return "welcome"; }
    void configure(tgpp::ext::Router& router) override;
};
} // namespace example
