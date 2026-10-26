#pragma once
#include <tgpp/methods.hpp>
#include <chrono>
#include <filesystem>
#include <functional>

namespace showcase {
class Cancelled : public std::exception {
public:
    const char* what() const noexcept override { return "tour cancelled"; }
};

class DemoContext {
    const tgpp::Bot& bot_;
    std::int64_t chat_;
    std::filesystem::path assets_;
    std::function<void(std::chrono::milliseconds)> wait_;
public:
    DemoContext(const tgpp::Bot& bot, std::int64_t chat, std::filesystem::path assets,
                std::function<void(std::chrono::milliseconds)> wait)
        : bot_(bot), chat_(chat), assets_(std::move(assets)), wait_(std::move(wait)) {}
    const tgpp::Bot& bot() const noexcept { return bot_; }
    std::int64_t chat() const noexcept { return chat_; }
    tgpp::InputFile file(const std::string& name, std::string mime) const;
    void pause(std::chrono::milliseconds duration) const { wait_(duration); }
    template <class Function> auto call(Function&& function) const {
        wait_(std::chrono::milliseconds(1100));
        return std::forward<Function>(function)();
    }
    tgpp::Message text(std::string text) const;
};

struct Demo {
    std::string id;
    std::string title;
    std::string group;
    std::function<void(DemoContext&)> run;
};

class Catalog {
    std::vector<Demo> demos_;
public:
    void add(Demo demo);
    const Demo* find(const std::string& id) const;
    const std::vector<Demo>& demos() const noexcept { return demos_; }
    tgpp::InlineKeyboardMarkup keyboard() const;
};

Catalog make_catalog();
void add_text_demos(Catalog& catalog);
void add_media_demos(Catalog& catalog);
void add_interactive_demos(Catalog& catalog);
void add_location_demos(Catalog& catalog);
void add_advanced_demos(Catalog& catalog);
tgpp::InlineKeyboardButton button(std::string label, std::string data);
} // namespace showcase
