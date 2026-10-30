#pragma once
#include <asio/ip/tcp.hpp>
#include <functional>
#include <tgpp/async.hpp>
#include <tgpp/types.hpp>

namespace tgpp::ext {
class AsyncApplication;

struct WebhookOptions {
    std::string address = "127.0.0.1";
    unsigned short port = 8080;
    std::string path;
    std::string secret_token;
    std::size_t max_header_bytes = 16384;
    std::size_t max_body_bytes = 1048576;
    std::size_t max_connections = 128;
    std::chrono::seconds receive_timeout{10};
};

// HTTPS is terminated by a reverse proxy. The callback must durably enqueue or
// complete processing before returning; failure returns 503 for Telegram retry.
class WebhookServer {
    struct State;
    std::shared_ptr<State> state_;

  public:
    using Receiver = std::function<Task<void>(Update)>;
    WebhookServer(asio::any_io_executor executor, WebhookOptions options, Receiver receiver);
    WebhookServer(asio::any_io_executor executor, WebhookOptions options, AsyncApplication& app);
    ~WebhookServer();
    WebhookServer(const WebhookServer&) = delete;
    WebhookServer& operator=(const WebhookServer&) = delete;
    unsigned short port() const;
    Task<void> run();
    void request_stop();
};
} // namespace tgpp::ext
