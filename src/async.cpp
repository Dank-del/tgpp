#include <fstream>
#include <mutex>
#include <tgpp/async.hpp>

namespace tgpp {
Runtime::Runtime(RuntimeOptions options)
    : work_(asio::make_work_guard(context_)),
      blocking_(options.blocking_threads, options.blocking_capacity) {
    if (!options.executor_threads || !options.blocking_threads) {
        throw std::invalid_argument("runtime thread counts must be positive");
    }
    try {
        for (std::size_t i = 0; i < options.executor_threads; ++i) {
            threads_.emplace_back([this] { context_.run(); });
        }
    } catch (...) {
        stop();
        join();
        throw;
    }
}

Runtime::~Runtime() {
    stop();
    join();
}

void Runtime::stop() noexcept {
    work_.reset();
    context_.stop();
}

void Runtime::join() {
    for (auto& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    blocking_.join();
}

AsyncClient::AsyncClient(std::shared_ptr<AsyncTransport> transport)
    : transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("transport must not be null");
    }
}

Task<Json> AsyncClient::call(Request request) const {
    co_return decode_response(co_await transport_->perform(std::move(request)));
}

struct FileUploadSource::Impl {
    std::shared_ptr<BlockingPool> owner;
    BlockingPool& pool;
    std::ifstream file;
    std::string path;
    std::mutex mutex;

    Impl(BlockingPool& pool, std::string path) : pool(pool), path(std::move(path)) {}
};

FileUploadSource::FileUploadSource(BlockingPool& pool, std::string path)
    : impl_(std::make_shared<Impl>(pool, std::move(path))) {}

FileUploadSource::FileUploadSource(std::shared_ptr<BlockingPool> pool, std::string path)
    : FileUploadSource(pool ? *pool : throw std::invalid_argument("file pool must not be null"),
                       std::move(path)) {
    impl_->owner = std::move(pool);
}

Task<std::vector<std::uint8_t>> FileUploadSource::read(std::size_t max_bytes) {
    auto state = impl_;
    co_return co_await state->pool.run([state, max_bytes] {
        std::lock_guard lock(state->mutex);
        if (!state->file.is_open()) {
            state->file.open(state->path, std::ios::binary);
        }
        if (!state->file.is_open()) {
            throw TransportError("could not open upload file");
        }
        std::vector<std::uint8_t> bytes(std::min(max_bytes, std::size_t(64 * 1024)));
        state->file.read(reinterpret_cast<char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(state->file.gcount()));
        if (state->file.bad()) {
            throw TransportError("could not read upload file");
        }
        return bytes;
    });
}

struct FileDownloadSink::Impl {
    std::shared_ptr<BlockingPool> owner;
    BlockingPool& pool;
    std::ofstream file;
    std::string path;
    std::mutex mutex;

    Impl(BlockingPool& pool, std::string path) : pool(pool), path(std::move(path)) {}
};

FileDownloadSink::FileDownloadSink(BlockingPool& pool, std::string path)
    : impl_(std::make_shared<Impl>(pool, std::move(path))) {}

FileDownloadSink::FileDownloadSink(std::shared_ptr<BlockingPool> pool, std::string path)
    : FileDownloadSink(pool ? *pool : throw std::invalid_argument("file pool must not be null"),
                       std::move(path)) {
    impl_->owner = std::move(pool);
}

Task<void> FileDownloadSink::write(std::vector<std::uint8_t> bytes) {
    auto state = impl_;
    co_await state->pool.run([state, bytes = std::move(bytes)] {
        std::lock_guard lock(state->mutex);
        if (!state->file.is_open()) {
            state->file.open(state->path, std::ios::binary | std::ios::trunc);
        }
        if (!state->file.is_open()) {
            throw TransportError("could not open download file");
        }
        state->file.write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
        state->file.flush();
        if (!state->file) {
            throw TransportError("could not write download file");
        }
        return true;
    });
}
} // namespace tgpp
