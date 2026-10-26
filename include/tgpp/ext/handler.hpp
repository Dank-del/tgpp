#pragma once
#include "context.hpp"
#include <functional>

namespace tgpp::ext {
using Filter = std::function<bool(const Context&)>;
using Callback = std::function<void(Context&)>;

class Handler {
    std::string name_;

  public:
    explicit Handler(std::string name);
    virtual ~Handler() = default;

    std::string_view name() const noexcept {
        return name_;
    }

    virtual bool matches(const Context& context) const = 0;
    virtual void handle(Context& context) = 0;
};

// Derive and override handle() for a class per command.
class CommandHandler : public Handler {
    std::string command_;

  public:
    explicit CommandHandler(std::string command);
    bool matches(const Context& context) const override;
};

class CallbackQueryHandler : public Handler {
    std::string prefix_;

  public:
    explicit CallbackQueryHandler(std::string prefix);
    bool matches(const Context& context) const override;
};

class FunctionHandler final : public Handler {
    Filter filter_;
    Callback callback_;

  public:
    FunctionHandler(std::string name, Filter filter, Callback callback);
    bool matches(const Context& context) const override;
    void handle(Context& context) override;
};

namespace filters {
Filter all();
Filter message();
Filter text();
Filter private_chat();
Filter command(std::string name);
Filter callback_prefix(std::string prefix);
Filter both(Filter first, Filter second);
Filter either(Filter first, Filter second);
Filter negate(Filter filter);
} // namespace filters
} // namespace tgpp::ext
