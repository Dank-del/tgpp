#include <asio.hpp>
#include <future>
#include <iostream>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/stores.hpp>
#include <tgpp/ext/webhook.hpp>
#include <thread>
using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::string http_exchange(unsigned short port, const std::vector<std::string>& chunks) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    for (const auto& chunk : chunks) {
        asio::write(socket, asio::buffer(chunk));
    }
    std::string response;
    std::array<char, 1024> data{};
    std::error_code ec;
    for (;;) {
        auto n = socket.read_some(asio::buffer(data), ec);
        response.append(data.data(), n);
        if (ec) {
            break;
        }
    }
    return response;
}

std::string http_exchange(unsigned short port, std::string request) {
    return http_exchange(port, std::vector<std::string>{std::move(request)});
}

std::string request(std::string token, std::string body, std::string path = "/updates") {
    return "POST " + path +
           " HTTP/1.1\r\nHost: localhost\r\nX-Telegram-Bot-Api-Secret-Token: " + token +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

class NoNetwork final : public AsyncTransport {
  public:
    Task<Json> perform(Request) override {
        throw std::logic_error("unexpected network request");
        co_return Json{};
    }
};

struct Fixture {
    asio::io_context io;
    MemoryStores store;
    std::atomic<int> inserted{0};
    WebhookServer server;
    std::future<void> done;
    std::jthread worker;

    explicit Fixture(WebhookOptions options)
        : server(io.get_executor(),
                 std::move(options),
                 [this](const Update& update) -> Task<void> {
                     if (update.update_id == 43) {
                         throw std::runtime_error("durable store unavailable");
                     }
                     std::vector<InboxUpdate> batch;
                     batch.push_back({update.update_id, "chat", to_json(update)});
                     inserted +=
                         static_cast<int>(co_await store.enqueue("webhook", std::move(batch)));
                 }),
          done(asio::co_spawn(io, server.run(), asio::use_future)), worker([this] { io.run(); }) {}

    ~Fixture() {
        server.request_stop();
        io.stop();
        worker.join();
    }
};
} // namespace

int main() {
    try {
        WebhookOptions options;
        options.port = 0;
        options.path = "/updates";
        options.secret_token = "test-secret";
        options.max_body_bytes = 1024;
        options.max_header_bytes = 512;
        options.receive_timeout = std::chrono::seconds(1);
        {
            asio::io_context io;
            AsyncApplication app(io.get_executor(), std::make_shared<NoNetwork>());
            app.claim_ingestion("polling");
            bool rejected = false;
            try {
                WebhookServer conflict(io.get_executor(), options, app);
            } catch (const std::logic_error&) {
                rejected = true;
            }
            require(rejected, "polling and webhook cannot share application");
        }
        Fixture fixture(options);
        auto port = fixture.server.port();
        const auto body = std::string(R"({"update_id":42})");
        require(http_exchange(port, request("test-secret", body)).starts_with("HTTP/1.1 200"),
                "valid delivery");
        require(http_exchange(port, request("test-secret", body)).starts_with("HTTP/1.1 200"),
                "duplicate delivery acknowledged");
        require(fixture.inserted == 1, "duplicate delivery durably deduplicated");
        require(http_exchange(port, request("bad", body)).starts_with("HTTP/1.1 403"),
                "secret rejected");
        require(
            http_exchange(port, request("test-secret", body, "/wrong")).starts_with("HTTP/1.1 404"),
            "path rejected");
        require(http_exchange(port, request("test-secret", "no json")).starts_with("HTTP/1.1 400"),
                "invalid json");
        require(http_exchange(port, request("test-secret", std::string(1025, 'x')))
                    .starts_with("HTTP/1.1 413"),
                "body limit");
        require(http_exchange(port, request("test-secret", R"({"update_id":43})"))
                    .starts_with("HTTP/1.1 503"),
                "failed durable admission is not acknowledged");
        auto duplicate = request("test-secret", body);
        auto pos = duplicate.find("Content-Length");
        duplicate.insert(pos, "X-Telegram-Bot-Api-Secret-Token: test-secret\r\n");
        require(http_exchange(port, duplicate).starts_with("HTTP/1.1 400"),
                "duplicate secret header");
        auto smuggled = request("test-secret", body);
        smuggled.insert(smuggled.find("Content-Length"), "Transfer-Encoding: chunked\r\n");
        require(http_exchange(port, smuggled).starts_with("HTTP/1.1 400"),
                "conflicting transfer framing rejected");
        auto oversized = request("test-secret", body);
        oversized.insert(oversized.find("Content-Length"),
                         "X-Padding: " + std::string(600, 'x') + "\r\n");
        require(http_exchange(port, oversized).starts_with("HTTP/1.1 431"), "header limit");
        const auto chunked = std::string("POST /updates HTTP/1.1\r\nHost: localhost\r\n") +
                             "X-Telegram-Bot-Api-Secret-Token: test-secret\r\n" +
                             "Transfer-Encoding: chunked\r\n\r\n10\r\n" + body + "\r\n0\r\n\r\n";
        std::vector<std::string> fragments;
        for (char c : chunked) {
            fragments.emplace_back(1, c);
        }
        require(http_exchange(port, fragments).starts_with("HTTP/1.1 200"),
                "fragmented chunked delivery");
        require(http_exchange(port, request("test-secret", R"({"update_id":-1})"))
                    .starts_with("HTTP/1.1 400"),
                "invalid update identifier");
        const auto stalled = http_exchange(port, "POST /updates HTTP/1.1\r\nHost: localhost\r\n");
        require(!stalled.starts_with("HTTP/1.1 200"), "incomplete request deadline");
        require(fixture.inserted == 1, "rejected requests never inserted");
        fixture.server.request_stop();
        fixture.done.get();
        std::cout
            << "Webhook authentication, parsing, deadline, and durable admission checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
