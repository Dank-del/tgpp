#pragma once
#include "router.hpp"

namespace tgpp::ext {
// A feature owns registration and optional lifecycle work. Inject dependencies
// in its constructor. configure() receives a router scoped to that feature.
class Module {
public:
    virtual ~Module() = default;
    virtual std::string name() const = 0;
    virtual void configure(Router& router) = 0;
    virtual void start() {}
    virtual void stop() noexcept {}
};
} // namespace tgpp::ext
