#pragma once
#include "context.hpp"
#include <functional>

namespace tgpp::ext {
enum class Result { unhandled, handled, stopped, failed };

// A synchronous continuation. Copies share the same one-call limit. Calling
// it after the middleware returns is rejected rather than using stale state.
class Next {
    struct State {
        bool active = true;
        bool called = false;
        std::function<Result()> continuation;
    };
    std::shared_ptr<State> state_;
    explicit Next(std::shared_ptr<State> state) : state_(std::move(state)) {}
    friend class Router;
public:
    Result operator()() const;
};

class Middleware {
public:
    virtual ~Middleware() = default;
    virtual Result handle(Context& context, Next next) = 0;
};
} // namespace tgpp::ext
