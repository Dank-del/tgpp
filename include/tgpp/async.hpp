#pragma once
#include "core.hpp"
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <thread>

namespace tgpp {
template <class T = void> using Task = asio::awaitable<T>;

// Admission is bounded independently of the fixed worker count. The pool must
// outlive all its run() tasks; returned values and exceptions resume on the caller.
class BlockingPool {
    asio::thread_pool pool_;
    std::atomic<std::size_t> pending_{0};
    std::size_t capacity_;

  public:
    explicit BlockingPool(std::size_t threads = 2, std::size_t capacity = 1024)
        : pool_(threads ? threads : 1), capacity_(capacity) {
        if (!threads || !capacity) {
            throw std::invalid_argument("blocking pool limits must be positive");
        }
    }

    void stop() {
        pool_.stop();
    }

    void join() {
        pool_.join();
    }

    template <class F> Task<std::invoke_result_t<F>> run(F function) {
        using Result = std::invoke_result_t<F>;
        using Value = std::conditional_t<std::is_void_v<Result>, bool, Result>;
        auto executor = co_await asio::this_coro::executor;
        if (pending_.fetch_add(1) >= capacity_) {
            --pending_;
            throw OverloadError();
        }

        struct Admission {
            std::atomic<std::size_t>& pending;
            std::atomic<bool> released{false};

            void release() {
                if (!released.exchange(true)) {
                    --pending;
                }
            }

            ~Admission() {
                release();
            }
        };

        auto admission = std::make_shared<Admission>(pending_);
        auto token = asio::use_awaitable;
        auto value = co_await asio::async_initiate<decltype(token),
                                                   void(std::exception_ptr, std::optional<Value>)>(
            [this, executor, admission, function = std::move(function)](auto handler) mutable {
                // Own initiation inputs outside the awaitable's initiation
                // frame before another thread can complete this operation.
                auto* worker_pool = &pool_;
                auto caller = executor;
                auto permit = admission;
                auto callable = std::make_shared<F>(std::move(function));
                auto completion = std::make_shared<decltype(handler)>(std::move(handler));
                auto work = std::make_shared<asio::executor_work_guard<asio::any_io_executor>>(
                    asio::make_work_guard(caller));
                asio::post(
                    *worker_pool,
                    [caller, permit, work, completion, callable = std::move(callable)]() mutable {
                        std::exception_ptr error;
                        std::optional<Value> result;
                        try {
                            if constexpr (std::is_void_v<Result>) {
                                (*callable)();
                                result = true;
                            } else {
                                result = (*callable)();
                            }
                        } catch (...) {
                            error = std::current_exception();
                        }
                        // A callable may own its pool (e.g. a file adapter).
                        // Move it to the caller before posting completion so its
                        // last owner can never destroy/join the pool on a worker.
                        auto finish = [permit,
                                       work,
                                       completion,
                                       callable = std::move(callable),
                                       error,
                                       result = std::move(result)]() mutable {
                            permit->release();
                            (*completion)(error, std::move(result));
                            work->reset();
                        };
                        asio::post(caller, std::move(finish));
                    });
            },
            token);
        if constexpr (std::is_void_v<Result>) {
            co_return;
        } else {
            co_return std::move(*value);
        }
    }
};

struct RuntimeOptions {
    std::size_t executor_threads = 2;
    std::size_t blocking_threads = 2;
    std::size_t blocking_capacity = 1024;
};

// A runtime owns its threads. Destroy it from an external thread after joining
// application tasks; callbacks and application dependencies must outlive tasks.
class Runtime {
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    BlockingPool blocking_;
    std::vector<std::thread> threads_;

  public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    asio::any_io_executor executor() {
        return context_.get_executor();
    }

    BlockingPool& blocking_pool() {
        return blocking_;
    }

    void stop() noexcept;
    void join();
};

class AsyncTransport {
  public:
    virtual ~AsyncTransport() = default;
    virtual Task<Json> perform(Request request) = 0;
};

class AsyncClient {
    std::shared_ptr<AsyncTransport> transport_;

  public:
    explicit AsyncClient(std::shared_ptr<AsyncTransport> transport);
    Task<Json> call(Request request) const;
};

// Each call returns at most max_bytes; an empty chunk is EOF. Sources must
// serialize access themselves if deliberately reused by multiple requests.
class AsyncUploadSource {
  public:
    virtual ~AsyncUploadSource() = default;
    virtual Task<std::vector<std::uint8_t>> read(std::size_t max_bytes) = 0;
};

class AsyncDownloadSink {
  public:
    virtual ~AsyncDownloadSink() = default;
    virtual Task<void> write(std::vector<std::uint8_t> bytes) = 0;
};

// File adapters execute blocking file operations on a supplied bounded pool.
class FileUploadSource final : public AsyncUploadSource {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    FileUploadSource(BlockingPool& pool, std::string path);
    FileUploadSource(std::shared_ptr<BlockingPool> pool, std::string path);
    Task<std::vector<std::uint8_t>> read(std::size_t max_bytes) override;
};

class FileDownloadSink final : public AsyncDownloadSink {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    FileDownloadSink(BlockingPool& pool, std::string path);
    FileDownloadSink(std::shared_ptr<BlockingPool> pool, std::string path);
    Task<void> write(std::vector<std::uint8_t> bytes) override;
};
} // namespace tgpp
