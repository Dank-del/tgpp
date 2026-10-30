#include <algorithm>
#include <asio.hpp>
#include <cctype>
#include <llhttp.h>
#include <map>
#include <set>
#include <tgpp/ext/async.hpp>
#include <tgpp/ext/webhook.hpp>

namespace tgpp::ext {
namespace {
bool equal_secret(std::string_view a, std::string_view b) {
    std::size_t difference = a.size() ^ b.size();
    for (std::size_t i = 0; i < b.size(); ++i) {
        difference |=
            static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^ static_cast<unsigned char>(b[i]);
    }
    return difference == 0;
}

struct Parsed {
    llhttp_t parser{};
    llhttp_settings_t settings{};
    std::string url, field, value, body;
    std::map<std::string, std::string> headers;
    const WebhookOptions& options;
    bool headers_done = false, complete = false;
    int error = 400;
    std::size_t header_bytes = 0;

    explicit Parsed(const WebhookOptions& o) : options(o) {
        llhttp_settings_init(&settings);
        settings.on_url = [](llhttp_t* p, const char* s, std::size_t n) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            x.url.append(s, n);
            return 0;
        };
        settings.on_header_field = [](llhttp_t* p, const char* s, std::size_t n) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            x.field.append(s, n);
            return 0;
        };
        settings.on_header_value = [](llhttp_t* p, const char* s, std::size_t n) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            x.value.append(s, n);
            return 0;
        };
        settings.on_header_value_complete = [](llhttp_t* p) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            std::transform(x.field.begin(), x.field.end(), x.field.begin(), [](unsigned char c) {
                return std::tolower(c);
            });
            if (!x.headers.emplace(x.field, x.value).second) {
                return HPE_USER;
            }
            x.field.clear();
            x.value.clear();
            return 0;
        };
        settings.on_headers_complete = [](llhttp_t* p) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            x.headers_done = true;
            if (p->method != HTTP_POST) {
                x.error = 405;
                return HPE_USER;
            }
            if (x.url != x.options.path) {
                x.error = 404;
                return HPE_USER;
            }
            if (!equal_secret(x.headers["x-telegram-bot-api-secret-token"],
                              x.options.secret_token)) {
                x.error = 403;
                return HPE_USER;
            }
            if (p->content_length != UINT64_MAX && p->content_length > x.options.max_body_bytes) {
                x.error = 413;
                return HPE_USER;
            }
            return 0;
        };
        settings.on_body = [](llhttp_t* p, const char* s, std::size_t n) -> int {
            auto& x = *static_cast<Parsed*>(p->data);
            if (n > x.options.max_body_bytes - x.body.size()) {
                x.error = 413;
                return HPE_USER;
            }
            x.body.append(s, n);
            return 0;
        };
        settings.on_message_complete = [](llhttp_t* p) -> int {
            static_cast<Parsed*>(p->data)->complete = true;
            return HPE_PAUSED;
        };
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        parser.data = this;
    }
};
} // namespace

struct WebhookServer::State : std::enable_shared_from_this<State> {
    asio::strand<asio::any_io_executor> executor;
    asio::ip::tcp::acceptor acceptor;
    WebhookOptions options;
    Receiver receiver;
    std::set<std::shared_ptr<asio::ip::tcp::socket>> sockets;
    std::map<std::shared_ptr<asio::ip::tcp::socket>, std::shared_ptr<asio::cancellation_signal>>
        cancellations;
    bool stopping = false, running = false;

    State(asio::any_io_executor e, WebhookOptions o, Receiver r)
        : executor(asio::make_strand(e)), acceptor(executor), options(std::move(o)),
          receiver(std::move(r)) {
        if (!receiver || options.path.empty() || options.path.front() != '/' ||
            options.secret_token.empty() || !options.max_connections || !options.max_body_bytes ||
            !options.max_header_bytes || options.receive_timeout.count() <= 0) {
            throw std::invalid_argument("invalid webhook options");
        }
        if (options.secret_token.size() > 256 ||
            options.secret_token.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                std::string::npos) {
            throw std::invalid_argument("invalid webhook secret token");
        }
        asio::ip::tcp::endpoint endpoint(asio::ip::make_address(options.address), options.port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();
    }

    static Task<void> serve(std::shared_ptr<State> self,
                            std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto deadline = std::make_shared<asio::steady_timer>(self->executor);
        deadline->expires_after(self->options.receive_timeout);
        deadline->async_wait([socket](std::error_code ec) {
            if (!ec) {
                std::error_code ignored;
                socket->close(ignored);
            }
        });
        int status = 400;
        try {
            Parsed parsed(self->options);
            std::array<char, 8192> buffer{};
            bool invalid = false;
            while (!parsed.complete) {
                auto n =
                    co_await socket->async_read_some(asio::buffer(buffer), asio::use_awaitable);
                // Feed headers bytewise so the configured header limit excludes the body.
                std::size_t i = 0;
                for (; i < n && !parsed.complete && !parsed.headers_done; ++i) {
                    if (++parsed.header_bytes > self->options.max_header_bytes) {
                        parsed.error = 431;
                        invalid = true;
                        break;
                    }
                    const auto result = llhttp_execute(&parsed.parser, buffer.data() + i, 1);
                    if (result != HPE_OK && result != HPE_PAUSED) {
                        invalid = true;
                        break;
                    }
                }
                if (!invalid && !parsed.complete && i < n) {
                    const auto result = llhttp_execute(&parsed.parser, buffer.data() + i, n - i);
                    invalid = result != HPE_OK && result != HPE_PAUSED;
                }
                if (invalid) {
                    break;
                }
            }
            deadline->cancel();
            status = parsed.error;
            if (parsed.complete && !invalid) {
                std::optional<Update> update;
                try {
                    update = from_json<Update>(Json::parse(parsed.body));
                } catch (const std::exception&) {
                }
                if (update && update->update_id >= 0) {
                    status = 503;
                    try {
                        co_await self->receiver(std::move(*update));
                        status = 200;
                    } catch (const std::exception&) {
                    }
                } else {
                    status = 400;
                }
            }
        } catch (const std::exception&) {
        }
        deadline->cancel();
        std::string response = "HTTP/1.1 " + std::to_string(status) +
                               " Result\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        std::error_code ignored;
        co_await asio::async_write(
            *socket, asio::buffer(response), asio::redirect_error(asio::use_awaitable, ignored));
        socket->close(ignored);
        self->sockets.erase(socket);
    }

    static Task<void> listen(std::shared_ptr<State> self) {
        if (self->running) {
            throw std::logic_error("webhook already running");
        }
        self->running = true;
        while (!self->stopping) {
            auto socket = std::make_shared<asio::ip::tcp::socket>(self->executor);
            std::error_code ec;
            co_await self->acceptor.async_accept(*socket,
                                                 asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                if (self->stopping) {
                    break;
                }
                throw std::system_error(ec);
            }
            if (self->sockets.size() >= self->options.max_connections) {
                // Closing before admission leaves delivery unacknowledged for retry.
                socket->close(ec);
                continue;
            }
            self->sockets.insert(socket);
            auto cancellation = std::make_shared<asio::cancellation_signal>();
            self->cancellations.emplace(socket, cancellation);
            asio::co_spawn(
                self->executor,
                serve(self, socket),
                asio::bind_cancellation_slot(cancellation->slot(),
                                             [self, socket, cancellation](std::exception_ptr) {
                                                 std::error_code ignored;
                                                 socket->close(ignored);
                                                 self->sockets.erase(socket);
                                                 self->cancellations.erase(socket);
                                             }));
        }
    }
};

WebhookServer::WebhookServer(asio::any_io_executor e, WebhookOptions o, Receiver r)
    : state_(std::make_shared<State>(e, std::move(o), std::move(r))) {}

WebhookServer::WebhookServer(asio::any_io_executor e, WebhookOptions o, AsyncApplication& app)
    : WebhookServer(e, std::move(o), [&app](Update update) -> Task<void> {
          co_await app.process(std::move(update));
      }) {
    app.claim_ingestion("webhook");
}

WebhookServer::~WebhookServer() {
    request_stop();
}

unsigned short WebhookServer::port() const {
    return state_->acceptor.local_endpoint().port();
}

Task<void> WebhookServer::run() {
    co_await asio::co_spawn(state_->executor, State::listen(state_), asio::use_awaitable);
}

void WebhookServer::request_stop() {
    asio::dispatch(state_->executor, [s = state_] {
        s->stopping = true;
        std::error_code ignored;
        s->acceptor.close(ignored);
        for (const auto& socket : s->sockets) {
            socket->close(ignored);
        }
        auto signals = s->cancellations;
        for (const auto& [socket, signal] : signals) {
            signal->emit(asio::cancellation_type::terminal);
        }
    });
}
} // namespace tgpp::ext
