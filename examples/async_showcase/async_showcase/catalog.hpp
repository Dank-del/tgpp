#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <tgpp/async_methods.hpp>

namespace async_showcase {
class AsyncDemoContext {
    const tgpp::AsyncBot& bot_;
    std::int64_t chat_;
    std::filesystem::path assets_;
    std::function<tgpp::Task<void>(std::chrono::milliseconds)> wait_;

  public:
    AsyncDemoContext(const tgpp::AsyncBot& bot,
                     std::int64_t chat,
                     std::filesystem::path assets,
                     std::function<tgpp::Task<void>(std::chrono::milliseconds)> wait)
        : bot_(bot), chat_(chat), assets_(std::move(assets)), wait_(std::move(wait)) {}

    const tgpp::AsyncBot& bot() const noexcept {
        return bot_;
    }

    std::int64_t chat() const noexcept {
        return chat_;
    }

    tgpp::InputFile file(const std::string& name, std::string mime) const;

    tgpp::Task<void> pause(std::chrono::milliseconds duration) const {
        co_await wait_(duration);
    }

    template <class Function>
    auto call(Function function) const -> tgpp::Task<typename decltype(function())::value_type> {
        co_await wait_(std::chrono::milliseconds(1100));
        co_return co_await function();
    }

    tgpp::Task<tgpp::Message> text(std::string text) const;
};

struct Demo {
    std::string id, title, group;
    std::function<tgpp::Task<void>(AsyncDemoContext&)> run;
};

class Catalog {
    std::vector<Demo> demos_;

  public:
    void add(Demo demo);
    const Demo* find(const std::string& id) const;

    const std::vector<Demo>& demos() const noexcept {
        return demos_;
    }

    tgpp::InlineKeyboardMarkup keyboard() const;
};

Catalog make_catalog();
void add_text_demos(Catalog&);
void add_media_demos(Catalog&);
void add_interactive_demos(Catalog&);
void add_location_demos(Catalog&);
void add_advanced_demos(Catalog&);
tgpp::InlineKeyboardButton button(std::string label, std::string data);
} // namespace async_showcase
