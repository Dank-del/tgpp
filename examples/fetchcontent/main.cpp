#include <tgpp/tgpp.hpp>
#include <tgpp/ext/ext.hpp>
#include <iostream>

class OfflineTransport final : public tgpp::Transport {
public:
    tgpp::Json perform(const tgpp::Request&) override {
        return {{"ok", true}, {"result", {{"id", 123}, {"is_bot", true}, {"first_name", "FetchContent example"}}}};
    }
};

int main() {
    // No token is needed to verify that the fetched library compiles and links.
    tgpp::ext::Application app(std::make_shared<OfflineTransport>());
    std::cout << app.bot().get_me().first_name << ": " << tgpp::api_version << '\n';
}
