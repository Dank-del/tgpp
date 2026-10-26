#include "catalog.hpp"
#include <algorithm>

namespace showcase {
tgpp::InputFile DemoContext::file(const std::string& name, std::string mime) const {
    const auto path = assets_ / name;
    if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("missing showcase asset: " + name);
    return {path.string(), name, std::move(mime)};
}
tgpp::Message DemoContext::text(std::string text) const {
    tgpp::SendMessageRequest request{}; request.chat_id = chat_; request.text = std::move(text);
    request.disable_notification = true;
    return call([&] { return bot_.send_message(request); });
}
void Catalog::add(Demo demo) {
    if (demo.id.empty() || demo.id.size() > 40 || !demo.run || find(demo.id)) throw std::invalid_argument("invalid or duplicate demo");
    demos_.push_back(std::move(demo));
}
const Demo* Catalog::find(const std::string& id) const {
    auto found = std::find_if(demos_.begin(), demos_.end(), [&](const Demo& demo) { return demo.id == id; });
    return found == demos_.end() ? nullptr : &*found;
}
tgpp::InlineKeyboardButton button(std::string label, std::string data) {
    tgpp::InlineKeyboardButton result{}; result.text = std::move(label); result.callback_data = std::move(data); return result;
}
tgpp::InlineKeyboardMarkup Catalog::keyboard() const {
    tgpp::InlineKeyboardMarkup keyboard{};
    keyboard.inline_keyboard.push_back({button("▶ Run all", "show:tour"), button("■ Stop", "show:stop"), button("Report", "show:report")});
    for (std::size_t index = 0; index < demos_.size(); index += 2) {
        std::vector<tgpp::InlineKeyboardButton> row;
        for (auto i = index; i < std::min(index + 2, demos_.size()); ++i)
            row.push_back(button(demos_[i].title, "show:demo:" + demos_[i].id));
        keyboard.inline_keyboard.push_back(std::move(row));
    }
    return keyboard;
}
Catalog make_catalog() {
    Catalog catalog;
    add_text_demos(catalog); add_media_demos(catalog); add_interactive_demos(catalog);
    add_location_demos(catalog); add_advanced_demos(catalog);
    return catalog;
}
} // namespace showcase
