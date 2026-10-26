#include "config.hpp"
#include <cstdlib>
#include <stdexcept>

namespace example {
Config Config::from_environment() {
    const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token || !*token) {
        throw std::runtime_error("Set TELEGRAM_BOT_TOKEN");
    }
    return Config{token};
}
} // namespace example
