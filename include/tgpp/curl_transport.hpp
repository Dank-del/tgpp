#pragma once
#include "core.hpp"

namespace tgpp {
struct CurlOptions {
    std::string base_url = "https://api.telegram.org";
    long connect_timeout_seconds = 10;
    long timeout_seconds = 90; // Must exceed the getUpdates long polling timeout.
};

// Each call owns a separate easy handle. No mutable request state is shared.
class CurlTransport final : public Transport {
    std::string token_;
    CurlOptions options_;
public:
    explicit CurlTransport(std::string token, CurlOptions options = {});
    Json perform(const Request& request) override;
};
} // namespace tgpp
