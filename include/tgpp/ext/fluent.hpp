#pragma once
#include "../fluent.hpp"
#include "async.hpp"

namespace tgpp::ext {
inline AsyncCallback reply(std::string text) {
    return [text = std::move(text)](AsyncContext& context) -> Task<void> {
        co_await context.reply(text);
    };
}

inline AsyncCallback echo() {
    return [](AsyncContext& context) -> Task<void> {
        if (auto* message = context.message(); message && message->text) {
            co_await context.reply(*message->text);
        }
    };
}

inline AsyncCallback answer(std::string text = {}, bool alert = false) {
    return [text = std::move(text), alert](AsyncContext& context) -> Task<void> {
        co_await context.answer_callback(text, alert);
    };
}

namespace async_filters {
class Predicate {
    AsyncFilter filter_;

  public:
    explicit Predicate(AsyncFilter filter) : filter_(std::move(filter)) {}

    bool operator()(const AsyncContext& context) const {
        return filter_(context);
    }

    friend Predicate operator&(Predicate left, Predicate right) {
        return Predicate([left = std::move(left), right = std::move(right)](const AsyncContext& c) {
            return left(c) && right(c);
        });
    }

    friend Predicate operator|(Predicate left, Predicate right) {
        return Predicate([left = std::move(left), right = std::move(right)](const AsyncContext& c) {
            return left(c) || right(c);
        });
    }

    friend Predicate operator!(Predicate filter) {
        return Predicate(
            [filter = std::move(filter)](const AsyncContext& c) { return !filter(c); });
    }
};

inline Predicate all() {
    return Predicate([](const AsyncContext&) { return true; });
}

inline Predicate message() {
    return Predicate([](const AsyncContext& c) { return c.message() != nullptr; });
}

inline Predicate text() {
    return Predicate(
        [](const AsyncContext& c) { return c.message() && c.message()->text.has_value(); });
}

inline Predicate private_chat() {
    return Predicate([](const AsyncContext& c) { return c.chat() && c.chat()->type == "private"; });
}

inline Predicate command(std::string name) {
    return Predicate([name = std::move(name)](const AsyncContext& c) {
        return c.command() && c.command()->name == name;
    });
}

inline Predicate callback_prefix(std::string prefix) {
    return Predicate([prefix = std::move(prefix)](const AsyncContext& c) {
        auto* q = c.callback_query();
        return q && q->data && q->data->starts_with(prefix);
    });
}
} // namespace async_filters
} // namespace tgpp::ext
