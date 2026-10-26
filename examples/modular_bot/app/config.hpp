#pragma once
#include <string>

namespace example {
struct Config {
    std::string bot_token;
    static Config from_environment();
};
} // namespace example
