#include <tgpp/ext/router.hpp>

namespace tgpp::ext {
Result Next::operator()() const {
    if (!state_ || !state_->active) {
        throw std::logic_error("middleware continuation has expired");
    }
    if (state_->called) {
        throw std::logic_error("middleware continuation can only run once");
    }
    state_->called = true;
    return state_->continuation();
}

Router::Router(std::string name, Filter filter)
    : name_(std::move(name)), filter_(std::move(filter)) {
    if (name_.empty() || name_.find('/') != std::string::npos) {
        throw std::invalid_argument("router name must be nonempty and contain no slash");
    }
}

void Router::ensure_configurable() const {
    if (frozen_) {
        throw std::logic_error(
            "routing is frozen; register handlers and middleware before starting");
    }
}

void Router::add(std::unique_ptr<Handler> handler) {
    ensure_configurable();
    if (!handler) {
        throw std::invalid_argument("handler must not be null");
    }
    routes_.push_back({std::move(handler), nullptr});
}

void Router::include(std::unique_ptr<Router> router) {
    ensure_configurable();
    if (!router) {
        throw std::invalid_argument("child router must not be null");
    }
    for (const auto& route : routes_) {
        if (route.router && route.router->name_ == router->name_) {
            throw std::invalid_argument("duplicate child router: " + router->name_);
        }
    }
    routes_.push_back({nullptr, std::move(router)});
}

Router& Router::child(std::string name, Filter filter) {
    auto value = std::make_unique<Router>(std::move(name), std::move(filter));
    auto& result = *value;
    include(std::move(value));
    return result;
}

void Router::use(std::unique_ptr<Middleware> middleware) {
    ensure_configurable();
    if (!middleware) {
        throw std::invalid_argument("middleware must not be null");
    }
    middleware_.push_back(std::move(middleware));
}

void Router::freeze() {
    if (frozen_) {
        return;
    }
    frozen_ = true;
    for (const auto& route : routes_) {
        if (route.router) {
            route.router->freeze();
        }
    }
}

Result Router::invoke(Context& context, std::size_t index) {
    if (index < middleware_.size()) {
        auto state = std::make_shared<Next::State>();
        state->continuation = [this, &context, index] { return invoke(context, index + 1); };
        try {
            const auto result = middleware_[index]->handle(context, Next(state));
            state->active = false;
            state->continuation = {};
            return result;
        } catch (...) {
            state->active = false;
            state->continuation = {};
            throw;
        }
    }
    for (const auto& route : routes_) {
        if (route.router) {
            const auto result = route.router->dispatch_at(context, context.route_);
            if (result != Result::unhandled) {
                return result;
            }
        } else {
            context.handler_ = std::string(route.handler->name());
            if (route.handler->matches(context)) {
                route.handler->handle(context);
                return Result::handled;
            }
            context.handler_.clear();
        }
    }
    return Result::unhandled;
}

Result Router::dispatch_at(Context& context, const std::string& parent) {
    auto previous_route = context.route_;
    auto previous_handler = context.handler_;
    context.route_ = parent.empty() ? name_ : parent + "/" + name_;
    context.handler_.clear();
    // Preserve failure location for Application's error handler on exceptions.
    auto result = !filter_ || filter_(context) ? invoke(context, 0) : Result::unhandled;
    context.route_ = std::move(previous_route);
    context.handler_ = std::move(previous_handler);
    return result;
}

Result Router::dispatch(Context& context) {
    freeze();
    return dispatch_at(context, "");
}
} // namespace tgpp::ext
