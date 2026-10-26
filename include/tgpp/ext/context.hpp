#pragma once
#include "../methods.hpp"
#include <any>
#include <string_view>
#include <typeindex>
#include <unordered_map>

namespace tgpp::ext {
struct Command {
    std::string name;
    std::string argument_text;
    std::vector<std::string> arguments;
};

// An update-scoped view. Never retain Context or pointers obtained from it.
class Context {
    const Bot& bot_;
    const Update& update_;
    std::optional<Command> command_;
    std::unordered_map<std::type_index, std::any> data_;
    std::string route_;
    std::string handler_;
    friend class Router;

  public:
    Context(const Bot& bot, const Update& update, std::string_view bot_username = {});
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const Bot& bot() const noexcept {
        return bot_;
    }

    const Update& update() const noexcept {
        return update_;
    }

    // Direct message updates only; callback messages do not become commands.
    const Message* message() const noexcept;
    const User* user() const noexcept;
    const Chat* chat() const noexcept;
    const CallbackQuery* callback_query() const noexcept;

    const std::optional<Command>& command() const noexcept {
        return command_;
    }

    std::string_view route() const noexcept {
        return route_;
    }

    std::string_view handler_name() const noexcept {
        return handler_;
    }

    Message reply(std::string text) const;
    bool answer_callback(std::string text = {}, bool alert = false) const;

    // Transient, typed metadata from middleware (e.g. an authenticated actor).
    // Long-lived services belong in handler constructors, not this map.
    template <class T> void set(T value) {
        data_.insert_or_assign(typeid(T), std::any(std::move(value)));
    }

    template <class T> T* find() noexcept {
        auto it = data_.find(typeid(T));
        return it == data_.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    template <class T> const T* find() const noexcept {
        auto it = data_.find(typeid(T));
        return it == data_.end() ? nullptr : std::any_cast<T>(&it->second);
    }

    template <class T> T& get() {
        auto* value = find<T>();
        if (!value) {
            throw std::logic_error("missing context metadata");
        }
        return *value;
    }
};
} // namespace tgpp::ext
