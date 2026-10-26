#pragma once
#include "handler.hpp"
#include "middleware.hpp"

namespace tgpp::ext {
// Routes (handlers and child routers) are tried in registration order. The
// first result other than unhandled wins. Middleware runs outermost first.
class Router {
    struct Route {
        std::unique_ptr<Handler> handler;
        std::unique_ptr<Router> router;
    };

    std::string name_;
    Filter filter_;
    std::vector<Route> routes_;
    std::vector<std::unique_ptr<Middleware>> middleware_;
    bool frozen_ = false;
    void ensure_configurable() const;
    Result invoke(Context& context, std::size_t index);
    Result dispatch_at(Context& context, const std::string& parent);

  public:
    explicit Router(std::string name = "root", Filter filter = {});
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    std::string_view name() const noexcept {
        return name_;
    }

    void add(std::unique_ptr<Handler> handler);
    void include(std::unique_ptr<Router> router);
    Router& child(std::string name, Filter filter = {});
    void use(std::unique_ptr<Middleware> middleware);

    template <class T, class... Args> T& add(Args&&... args) {
        auto value = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *value;
        add(std::move(value));
        return result;
    }

    template <class T, class... Args> T& use(Args&&... args) {
        auto value = std::make_unique<T>(std::forward<Args>(args)...);
        auto& result = *value;
        use(std::move(value));
        return result;
    }

    void freeze();
    Result dispatch(Context& context);
};
} // namespace tgpp::ext
