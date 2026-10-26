#pragma once
#include "core.hpp"

namespace tgpp {
struct CurlOptions {
    std::string base_url = "https://api.telegram.org";
    long connect_timeout_seconds = 10;
    long timeout_seconds = 90; // Must exceed the getUpdates long polling timeout.
    std::size_t max_connections = 8;
    long pool_timeout_seconds = 30;
};

// Exclusive leases reuse connections and TLS/DNS caches between calls.
class CurlTransport final : public Transport {
    struct Pool;
    std::string token_;
    CurlOptions options_;
    std::shared_ptr<Pool> pool_;
public:
    explicit CurlTransport(std::string token, CurlOptions options = {});
    Json perform(const Request& request) override;
};
} // namespace tgpp
