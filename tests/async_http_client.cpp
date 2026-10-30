#include <atomic>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <tgpp/async_curl.hpp>
#include <tgpp/async_methods.hpp>

using namespace tgpp;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Source final : public AsyncUploadSource {
    std::size_t remaining = 512 * 1024;

  public:
    Task<std::vector<std::uint8_t>> read(std::size_t max) override {
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        auto count = std::min(max, remaining);
        remaining -= count;
        co_return std::vector<std::uint8_t>(count, 'x');
    }
};

class Sink final : public AsyncDownloadSink {
  public:
    std::size_t count = 0;

    Task<void> write(std::vector<std::uint8_t> bytes) override {
        require(bytes.size() <= 64 * 1024, "download buffer not bounded");
        auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
        count += bytes.size();
    }
};

Task<void> check_api(std::shared_ptr<AsyncCurlTransport> transport) {
    AsyncBot bot(transport);
    auto user = co_await bot.get_me();
    require(user.id == 123 && user.is_bot, "generated async getMe failed");
    Request upload;
    upload.method = "upload";
    InputFile streamed;
    streamed.filename = "stream.bin";
    streamed.source = std::make_shared<Source>();
    upload.files.push_back({"document", std::move(streamed)});
    auto response = co_await transport->perform(std::move(upload));
    require(response.at("result").get<int>() > 512 * 1024, "streamed multipart truncated");
    Request memory;
    memory.method = "upload";
    memory.files.push_back({"document", InputFile::from_memory({1, 2, 3, 4}, "memory.bin")});
    require((co_await transport->perform(std::move(memory))).at("ok").get<bool>(),
            "memory upload failed");
    auto sink = std::make_shared<Sink>();
    co_await transport->download("documents/test.bin", sink);
    require(sink->count == 1024 * 1024, "download was truncated or duplicated");
    auto files = std::make_shared<BlockingPool>(2, 4);
    const auto path = std::filesystem::temp_directory_path() /
                      ("tgpp-file-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto file_sink = std::make_shared<FileDownloadSink>(files, path.string());
    co_await transport->download("documents/test.bin", file_sink);
    require(std::filesystem::file_size(path) == 1024 * 1024, "file sink truncated");
    Request file_upload;
    file_upload.method = "upload";
    InputFile file;
    file.path = path.string();
    file.filename = "test.bin";
    file_upload.files.push_back({"document", std::move(file)});
    auto file_response = co_await transport->perform(std::move(file_upload));
    require(file_response.at("result").get<int>() > 1024 * 1024, "path source truncated");
    file_sink.reset();
    std::filesystem::remove(path);

    Request deadline;
    deadline.method = "slow";
    deadline.control = std::make_shared<RequestControl>();
    deadline.control->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    bool timed_out = false;
    try {
        co_await transport->perform(std::move(deadline));
    } catch (const TimeoutError&) {
        timed_out = true;
    }
    require(timed_out, "deadline did not cancel transfer");
    std::stop_source stop;
    stop.request_stop();
    Request cancelled;
    cancelled.method = "slow";
    cancelled.control = std::make_shared<RequestControl>();
    cancelled.control->stop = stop.get_token();
    bool stopped = false;
    try {
        co_await transport->perform(std::move(cancelled));
    } catch (const CancelledError&) {
        stopped = true;
    }
    require(stopped, "stop token ignored");
    Request invalid;
    invalid.method = "badjson";
    bool malformed = false;
    try {
        co_await transport->perform(std::move(invalid));
    } catch (const ProtocolError&) {
        malformed = true;
    }
    require(malformed, "malformed response accepted");
    Request server_error;
    server_error.method = "servererror";
    bool retryable = false;
    try {
        co_await transport->perform(std::move(server_error));
    } catch (const TransportError& error) {
        retryable = error.retryable;
    }
    require(retryable, "server failure not retryable");
}

Task<void> shared_pool_cleanup() {
    struct Check {
        std::atomic<bool> destroyed{false}, wrong_thread{false};
    };

    struct Marker {
        std::shared_ptr<Check> check;
        std::thread::id expected;

        Marker(std::shared_ptr<Check> check, std::thread::id expected)
            : check(std::move(check)), expected(expected) {}

        ~Marker() {
            check->wrong_thread = std::this_thread::get_id() != expected;
            check->destroyed = true;
        }
    };

    auto executor = co_await asio::this_coro::executor;
    auto check = std::make_shared<Check>();
    BlockingPool pool(1, 4);
    auto marker = std::make_shared<Marker>(check, std::this_thread::get_id());
    auto callback = [marker = std::move(marker)] { return 7; };
    require(co_await pool.run(std::move(callback)) == 7, "tracked blocking work failed");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!check->destroyed && std::chrono::steady_clock::now() < deadline) {
        asio::steady_timer timer(executor, std::chrono::milliseconds(1));
        co_await timer.async_wait(asio::use_awaitable);
    }
    require(check->destroyed && !check->wrong_thread,
            "blocking callable destroyed on worker instead of caller");
    const auto path = std::filesystem::temp_directory_path() /
                      ("tgpp-owned-pool-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (int round = 0; round < 16; ++round) {
        auto sink =
            std::make_shared<FileDownloadSink>(std::make_shared<BlockingPool>(1, 2), path.string());
        auto chunk = std::vector<std::uint8_t>{1, 2, 3};
        co_await sink->write(std::move(chunk));
        sink.reset(); // The adapter owns the only pool; cleanup must not self-join.
        auto source =
            std::make_shared<FileUploadSource>(std::make_shared<BlockingPool>(1, 2), path.string());
        auto bytes = co_await source->read(64);
        source.reset();
        require(bytes == std::vector<std::uint8_t>({1, 2, 3}), "owned file pool lost bytes");
    }
    std::filesystem::remove(path);
}

Task<void> offload_check(BlockingPool& pool) {
    auto value = co_await pool.run([] { return std::string("owned result"); });
    require(value == "owned result", "blocking result lost");
    bool failed = false;
    try {
        co_await pool.run([]() -> int { throw std::runtime_error("expected"); });
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed, "blocking exception lost");
    co_await pool.run([] {});
}

Task<int> offload_slow(BlockingPool& pool) {
    co_return co_await pool.run([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 42;
    });
}

Task<void> slow(std::shared_ptr<AsyncCurlTransport> transport) {
    Request request;
    request.method = "slow";
    co_await transport->perform(std::move(request));
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected base URL");
        for (auto invalid :
             std::vector<Json>{Json{{"ok", true}},
                               Json{{"ok", "true"}},
                               Json{{"ok", false}, {"parameters", false}},
                               Json{{"ok", false}, {"parameters", {{"retry_after", "later"}}}}}) {
            bool rejected_envelope = false;
            try {
                decode_response(invalid);
            } catch (const ProtocolError&) {
                rejected_envelope = true;
            }
            require(rejected_envelope, "malformed envelope accepted");
        }
        asio::io_context io;
        BlockingPool pool(1, 1);
        auto ownership = asio::co_spawn(io, shared_pool_cleanup(), asio::use_future);
        io.run();
        ownership.get();
        io.restart();
        auto blocking = asio::co_spawn(io, offload_check(pool), asio::use_future);
        io.run();
        blocking.get();
        io.restart();
        auto accepted = asio::co_spawn(io, offload_slow(pool), asio::use_future);
        auto rejected = asio::co_spawn(io, offload_slow(pool), asio::use_future);
        io.run();
        require(accepted.get() == 42, "blocking job failed");
        bool overloaded = false;
        try {
            rejected.get();
        } catch (const OverloadError&) {
            overloaded = true;
        }
        require(overloaded, "blocking pool admission is not bounded");
        io.restart();

        AsyncCurlOptions options;
        options.base_url = argv[1];
        options.max_connections = 4;
        options.max_pending = 4;
        auto transport = std::make_shared<AsyncCurlTransport>("123:token", options);
        auto api = asio::co_spawn(io, check_api(transport), asio::use_future);
        io.run();
        api.get();
        require(transport->stats().active == 0, "active request leaked");
        io.restart();
        asio::cancellation_signal signal;
        auto active_cancel = asio::co_spawn(
            io, slow(transport), asio::bind_cancellation_slot(signal.slot(), asio::use_future));
        asio::steady_timer cancel_timer(io, std::chrono::milliseconds(30));
        cancel_timer.async_wait(
            [&](asio::error_code) { signal.emit(asio::cancellation_type::terminal); });
        io.run();
        bool coroutine_cancelled = false;
        try {
            active_cancel.get();
        } catch (const CancelledError&) {
            coroutine_cancelled = true;
        }
        require(coroutine_cancelled, "active coroutine cancellation ignored");
        io.restart();
        AsyncCurlOptions queued_options = options;
        queued_options.max_connections = 1;
        auto queued_transport = std::make_shared<AsyncCurlTransport>("123:token", queued_options);
        auto first = asio::co_spawn(io, slow(queued_transport), asio::use_future);
        asio::cancellation_signal queued_signal;
        auto second =
            asio::co_spawn(io,
                           slow(queued_transport),
                           asio::bind_cancellation_slot(queued_signal.slot(), asio::use_future));
        asio::steady_timer queued_timer(io, std::chrono::milliseconds(30));
        queued_timer.async_wait(
            [&](asio::error_code) { queued_signal.emit(asio::cancellation_type::terminal); });
        io.run();
        first.get();
        bool queue_cancelled = false;
        try {
            second.get();
        } catch (const CancelledError&) {
            queue_cancelled = true;
        }
        require(queue_cancelled, "queued coroutine cancellation ignored");

        io.restart();
        std::vector<std::future<void>> futures;
        for (int i = 0; i < 12; ++i) {
            futures.push_back(asio::co_spawn(io, slow(transport), asio::use_future));
        }
        io.run();
        int rejected_count = 0;
        for (auto& future : futures) {
            try {
                future.get();
            } catch (const OverloadError&) {
                ++rejected_count;
            }
        }
        require(rejected_count > 0, "overload did not reject requests");
        require(transport->stats().queued == 0 && transport->stats().active == 0,
                "queued operation leaked");
        std::cout << "async client/streaming/deadline/overload passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
