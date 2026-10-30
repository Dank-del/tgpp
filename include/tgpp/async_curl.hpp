#pragma once
#include "async.hpp"
#include "curl_transport.hpp"

namespace tgpp {
struct AsyncTransportEvent {
    std::string method;
    std::chrono::steady_clock::duration elapsed{};
    bool failed = false;
};

struct AsyncCurlOptions : CurlOptions {
    AsyncCurlOptions() {
        max_connections = 32;
    }

    std::size_t max_pending = 1024;
    std::size_t max_response_bytes = 64 * 1024 * 1024;
    // Executed on the initiating executor; must be nonblocking. Exceptions are ignored.
    std::function<void(const AsyncTransportEvent&)> observer;
};

struct AsyncTransportStats {
    std::size_t active = 0;
    std::size_t queued = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
};

class AsyncCurlTransport final : public AsyncTransport {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    explicit AsyncCurlTransport(std::string token, AsyncCurlOptions options = {});
    ~AsyncCurlTransport();
    Task<Json> perform(Request request) override;
    // The path returned by getFile, not an arbitrary URL. Redirects are disabled.
    Task<void> download(std::string file_path,
                        std::shared_ptr<AsyncDownloadSink> sink,
                        std::shared_ptr<RequestControl> control = {});
    AsyncTransportStats stats() const;
    void request_stop() noexcept;
};
} // namespace tgpp
