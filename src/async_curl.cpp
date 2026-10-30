#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <tgpp/async_curl.hpp>

namespace tgpp {
namespace {
bool safe_name(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

bool transient(CURLcode code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:
        return true;
    default:
        return false;
    }
}

void check(CURLcode code) {
    if (code != CURLE_OK) {
        throw TransportError(
            std::string("HTTP transport: ") + curl_easy_strerror(code), transient(code), code);
    }
}

void check_multi(CURLMcode code) {
    if (code != CURLM_OK) {
        throw TransportError(std::string("HTTP reactor: ") + curl_multi_strerror(code));
    }
}
} // namespace

struct AsyncCurlTransport::Impl : std::enable_shared_from_this<AsyncCurlTransport::Impl> {
    struct Operation;

    struct Upload {
        std::shared_ptr<AsyncUploadSource> source;
        std::vector<std::uint8_t> buffer;
        std::size_t offset = 0;
        bool reading = false, eof = false;
        Operation* operation = nullptr;
    };

    struct Operation : std::enable_shared_from_this<Operation> {
        Request request;
        std::string download_path;
        std::shared_ptr<AsyncDownloadSink> sink;
        asio::any_io_executor executor;
        std::function<void(std::exception_ptr, Json)> complete;
        asio::executor_work_guard<asio::any_io_executor> work;
        std::weak_ptr<Impl> owner;
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::exception_ptr stream_error;
        std::vector<std::uint8_t> download_buffer;
        bool writing = false, download_ready = false, paused = false;
        CURL* easy = nullptr;
        curl_mime* mime = nullptr;
        curl_slist* headers = nullptr;
        std::string url, body, response;
        std::vector<std::unique_ptr<Upload>> uploads;
        std::chrono::steady_clock::time_point queued_at = std::chrono::steady_clock::now();
        std::size_t max_response = 0;

        explicit Operation(asio::any_io_executor executor)
            : executor(executor), work(asio::make_work_guard(executor)) {}

        ~Operation() {
            if (easy) {
                curl_easy_cleanup(easy);
            }
            if (mime) {
                curl_mime_free(mime);
            }
            if (headers) {
                curl_slist_free_all(headers);
            }
        }
    };

    std::shared_ptr<BlockingPool> files = std::make_shared<BlockingPool>(2, 64);
    std::string token;
    AsyncCurlOptions options;
    long http_version = CURL_HTTP_VERSION_1_1;
    CURLM* multi = nullptr;
    std::mutex mutex;
    std::deque<std::shared_ptr<Operation>> pending;
    std::map<CURL*, std::shared_ptr<Operation>> active;
    std::thread thread;
    std::atomic<bool> closing{false};
    std::atomic<std::size_t> active_count{0}, queued_count{0};
    std::atomic<std::uint64_t> completed_count{0}, failed_count{0};

    Impl(std::string token, AsyncCurlOptions options)
        : token(std::move(token)), options(std::move(options)) {
        // libcurl's init is reference counted, paired once per reactor.
        check(curl_global_init(CURL_GLOBAL_DEFAULT));
        const auto* version = curl_version_info(CURLVERSION_NOW);
        if (version->features & CURL_VERSION_HTTP2) {
            http_version = CURL_HTTP_VERSION_2TLS;
        }
        if (!(version->features & CURL_VERSION_ASYNCHDNS)) {
            curl_global_cleanup();
            throw std::runtime_error("async transport requires libcurl asynchronous DNS support");
        }
        if (this->token.empty() ||
            this->token.find_first_not_of(
                "0123456789:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") !=
                std::string::npos) {
            curl_global_cleanup();
            throw std::invalid_argument("invalid bot token characters");
        }
        while (!this->options.base_url.empty() && this->options.base_url.back() == '/') {
            this->options.base_url.pop_back();
        }
        if ((this->options.base_url.rfind("https://", 0) != 0 &&
             this->options.base_url.rfind("http://", 0) != 0) ||
            !this->options.max_connections || !this->options.max_pending ||
            !this->options.max_response_bytes || this->options.timeout_seconds <= 0 ||
            this->options.connect_timeout_seconds <= 0 || this->options.pool_timeout_seconds <= 0) {
            curl_global_cleanup();
            throw std::invalid_argument("invalid async HTTP options");
        }
        multi = curl_multi_init();
        if (!multi) {
            curl_global_cleanup();
            throw std::bad_alloc();
        }
        curl_multi_setopt(multi,
                          CURLMOPT_MAX_TOTAL_CONNECTIONS,
                          static_cast<long>(this->options.max_connections));
        curl_multi_setopt(
            multi, CURLMOPT_MAX_HOST_CONNECTIONS, static_cast<long>(this->options.max_connections));
    }

    ~Impl() {
        stop();
        if (thread.joinable()) {
            thread.join();
        }
        curl_multi_cleanup(multi);
        curl_global_cleanup();
    }

    void start() {
        thread = std::thread([this] { run(); });
    }

    void stop() noexcept {
        closing.store(true);
        curl_multi_wakeup(multi);
    }

    void wake() noexcept {
        curl_multi_wakeup(multi);
    }

    static Task<void> read_upload(std::shared_ptr<Operation> op, Upload* upload) {
        std::exception_ptr error;
        std::vector<std::uint8_t> bytes;
        try {
            bytes = co_await upload->source->read(64 * 1024);
            if (bytes.size() > 64 * 1024) {
                throw ProtocolError("upload source exceeded chunk limit");
            }
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard lock(op->mutex);
            upload->buffer = std::move(bytes);
            upload->offset = 0;
            upload->eof = upload->buffer.empty();
            upload->reading = false;
            if (error) {
                op->stream_error = error;
            }
        }
        if (auto owner = op->owner.lock()) {
            owner->wake();
        }
    }

    static std::size_t
    read_callback(char* data, std::size_t size, std::size_t count, void* raw) noexcept {
        auto* upload = static_cast<Upload*>(raw);
        auto* op = upload->operation;
        try {
            std::lock_guard lock(op->mutex);
            if (op->cancelled || op->stream_error) {
                return CURL_READFUNC_ABORT;
            }
            const auto bytes = std::min(size * count, upload->buffer.size() - upload->offset);
            if (bytes) {
                std::copy_n(upload->buffer.data() + upload->offset,
                            bytes,
                            reinterpret_cast<std::uint8_t*>(data));
                upload->offset += bytes;
                return bytes;
            }
            if (upload->eof) {
                return 0;
            }
            if (!upload->reading) {
                upload->reading = true;
                asio::co_spawn(
                    op->executor, read_upload(op->shared_from_this(), upload), asio::detached);
            }
            op->paused = true;
            return CURL_READFUNC_PAUSE;
        } catch (...) {
            return CURL_READFUNC_ABORT;
        }
    }

    static Task<void> write_download(std::shared_ptr<Operation> op) {
        std::exception_ptr error;
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard lock(op->mutex);
            bytes = std::move(op->download_buffer);
        }
        try {
            co_await op->sink->write(std::move(bytes));
        } catch (...) {
            error = std::current_exception();
        }
        {
            std::lock_guard lock(op->mutex);
            op->writing = false;
            op->download_ready = true;
            if (error) {
                op->stream_error = error;
            }
        }
        if (auto owner = op->owner.lock()) {
            owner->wake();
        }
    }

    static std::size_t
    write_callback(char* data, std::size_t size, std::size_t count, void* raw) noexcept {
        auto* op = static_cast<Operation*>(raw);
        const auto bytes = size * count;
        try {
            long status = 0;
            curl_easy_getinfo(op->easy, CURLINFO_RESPONSE_CODE, &status);
            if (op->sink && status >= 200 && status < 300) {
                std::lock_guard lock(op->mutex);
                if (op->stream_error || op->cancelled) {
                    return 0;
                }
                if (op->download_ready) {
                    op->download_ready = false;
                    return bytes; // curl replays the paused buffer; it is already written.
                }
                if (!op->writing) {
                    if (bytes > 64 * 1024) {
                        return 0;
                    }
                    op->download_buffer.assign(data, data + bytes);
                    op->writing = true;
                    asio::co_spawn(
                        op->executor, write_download(op->shared_from_this()), asio::detached);
                }
                op->paused = true;
                return CURL_WRITEFUNC_PAUSE;
            }
            if (bytes > op->max_response - op->response.size()) {
                return 0;
            }
            op->response.append(data, bytes);
            return bytes;
        } catch (...) {
            return 0;
        }
    }

    void configure(const std::shared_ptr<Operation>& op) {
        op->easy = curl_easy_init();
        if (!op->easy) {
            throw std::bad_alloc();
        }
        op->max_response = options.max_response_bytes;
        op->url = options.base_url + (op->sink ? "/file/bot" : "/bot") + token + "/" +
                  (op->sink ? op->download_path : op->request.method);
        check(curl_easy_setopt(op->easy, CURLOPT_URL, op->url.c_str()));
        check(curl_easy_setopt(op->easy, CURLOPT_NOSIGNAL, 1L));
        check(curl_easy_setopt(op->easy, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds));
        check(curl_easy_setopt(op->easy, CURLOPT_TIMEOUT, options.timeout_seconds));
        check(curl_easy_setopt(op->easy, CURLOPT_WRITEFUNCTION, write_callback));
        check(curl_easy_setopt(op->easy, CURLOPT_WRITEDATA, op.get()));
        // Request HTTP/2 only when this libcurl actually implements it. A
        // build without HTTP/2 rejects 2TLS even for a plain HTTP request.
        check(curl_easy_setopt(op->easy, CURLOPT_HTTP_VERSION, http_version));
        if (op->sink) {
            return;
        }
        check(curl_easy_setopt(op->easy, CURLOPT_POST, 1L));
        if (op->request.files.empty()) {
            op->body = op->request.parameters.dump();
            op->headers = curl_slist_append(nullptr, "Content-Type: application/json");
            if (!op->headers) {
                throw std::bad_alloc();
            }
            check(curl_easy_setopt(op->easy, CURLOPT_HTTPHEADER, op->headers));
            check(curl_easy_setopt(op->easy, CURLOPT_POSTFIELDS, op->body.c_str()));
            check(curl_easy_setopt(
                op->easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(op->body.size())));
        } else {
            op->mime = curl_mime_init(op->easy);
            if (!op->mime) {
                throw std::bad_alloc();
            }
            std::set<std::string> names;
            for (auto it = op->request.parameters.begin(); it != op->request.parameters.end();
                 ++it) {
                names.insert(it.key());
                auto* part = curl_mime_addpart(op->mime);
                if (!part) {
                    throw std::bad_alloc();
                }
                check(curl_mime_name(part, it.key().c_str()));
                const auto value = it->is_string() ? it->get<std::string>() : it->dump();
                check(curl_mime_data(part, value.data(), value.size()));
            }
            for (const auto& attachment : op->request.files) {
                if (!safe_name(attachment.name) || !names.insert(attachment.name).second) {
                    throw std::invalid_argument("invalid or duplicate attachment name");
                }
                auto* part = curl_mime_addpart(op->mime);
                if (!part) {
                    throw std::bad_alloc();
                }
                check(curl_mime_name(part, attachment.name.c_str()));
                const auto& file = attachment.file;
                if (file.source || (!file.memory && !file.path.empty())) {
                    auto upload = std::make_unique<Upload>();
                    upload->source = file.source
                                         ? file.source
                                         : std::make_shared<FileUploadSource>(files, file.path);
                    upload->operation = op.get();
                    check(
                        curl_mime_data_cb(part, -1, read_callback, nullptr, nullptr, upload.get()));
                    op->uploads.push_back(std::move(upload));
                } else if (file.memory) {
                    check(curl_mime_data(part,
                                         reinterpret_cast<const char*>(file.memory->data()),
                                         file.memory->size()));
                } else if (!file.path.empty()) {
                    check(curl_mime_filedata(part, file.path.c_str()));
                } else {
                    throw std::invalid_argument("upload has no data source");
                }
                const auto filename = file.filename.empty()
                                          ? std::filesystem::path(file.path).filename().string()
                                          : file.filename;
                check(curl_mime_filename(part, filename.c_str()));
                check(curl_mime_type(part, file.content_type.c_str()));
            }
            check(curl_easy_setopt(op->easy, CURLOPT_MIMEPOST, op->mime));
        }
    }

    std::exception_ptr cancelled(const std::shared_ptr<Operation>& op, bool queued) {
        if (closing || op->cancelled ||
            (op->request.control && op->request.control->stop.stop_requested())) {
            return std::make_exception_ptr(CancelledError());
        }
        if (op->request.control && op->request.control->deadline &&
            std::chrono::steady_clock::now() >= *op->request.control->deadline) {
            return std::make_exception_ptr(TimeoutError());
        }
        if (queued && std::chrono::steady_clock::now() - op->queued_at >=
                          std::chrono::seconds(options.pool_timeout_seconds)) {
            return std::make_exception_ptr(TimeoutError());
        }
        std::lock_guard lock(op->mutex);
        return op->stream_error;
    }

    void finish(const std::shared_ptr<Operation>& op, std::exception_ptr error, Json value = {}) {
        ++completed_count;
        if (error) {
            ++failed_count;
        }
        asio::post(op->executor,
                   [op, error, observer = options.observer, value = std::move(value)]() mutable {
                       if (observer) {
                           try {
                               observer({op->sink ? "download" : op->request.method,
                                         std::chrono::steady_clock::now() - op->queued_at,
                                         bool(error)});
                           } catch (...) {
                           }
                       }
                       auto complete = std::move(op->complete);
                       complete(error, std::move(value));
                       op->work.reset();
                   });
    }

    void submit(std::shared_ptr<Operation> op) {
        // Rejection can complete on another executor before submit returns.
        // Keep the reactor alive through wakeup even if the caller releases
        // its transport as soon as that completion arrives.
        auto keep_alive = shared_from_this();
        std::exception_ptr error;
        {
            std::lock_guard lock(mutex);
            if (closing) {
                error = std::make_exception_ptr(CancelledError());
            } else if (queued_count.load() >=
                       options.max_pending -
                           ((options.max_pending > 1 && op->request.method != "getUpdates") ? 1
                                                                                            : 0)) {
                error = std::make_exception_ptr(OverloadError());
            } else {
                pending.push_back(op);
                ++queued_count;
            }
        }
        if (error) {
            finish(op, error);
        }
        wake();
    }

    void iteration() {
        std::deque<std::shared_ptr<Operation>> incoming;
        {
            std::lock_guard lock(mutex);
            incoming.swap(pending);
        }
        std::deque<std::shared_ptr<Operation>> deferred;
        for (auto& op : incoming) {
            if (auto error = cancelled(op, true)) {
                --queued_count;
                finish(op, error);
                continue;
            }
            const auto capacity =
                options.max_connections -
                ((options.max_connections > 1 && op->request.method != "getUpdates") ? 1 : 0);
            if (active.size() >= capacity) {
                deferred.push_back(op);
                continue;
            }
            --queued_count;
            try {
                configure(op);
                check_multi(curl_multi_add_handle(multi, op->easy));
                active.emplace(op->easy, op);
                ++active_count;
            } catch (...) {
                finish(op, std::current_exception());
            }
        }
        {
            std::lock_guard lock(mutex);
            pending.insert(pending.begin(), deferred.begin(), deferred.end());
        }
        for (auto it = active.begin(); it != active.end();) {
            auto op = it->second;
            if (auto error = cancelled(op, false)) {
                curl_multi_remove_handle(multi, it->first);
                it = active.erase(it);
                --active_count;
                finish(op, error);
            } else {
                bool resume = false;
                {
                    std::lock_guard lock(op->mutex);
                    if (op->paused && !op->writing &&
                        std::none_of(op->uploads.begin(), op->uploads.end(), [](const auto& u) {
                            return u->reading;
                        })) {
                        op->paused = false;
                        resume = true;
                    }
                }
                if (resume) {
                    check(curl_easy_pause(op->easy, CURLPAUSE_CONT));
                }
                ++it;
            }
        }
        int running = 0;
        check_multi(curl_multi_perform(multi, &running));
        int left = 0;
        while (auto* message = curl_multi_info_read(multi, &left)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            auto found = active.find(message->easy_handle);
            if (found == active.end()) {
                continue;
            }
            auto op = found->second;
            curl_multi_remove_handle(multi, op->easy);
            active.erase(found);
            --active_count;
            try {
                if (auto error = cancelled(op, false)) {
                    std::rethrow_exception(error);
                }
                check(message->data.result);
                long status = 0;
                check(curl_easy_getinfo(op->easy, CURLINFO_RESPONSE_CODE, &status));
                if (op->sink) {
                    if (status < 200 || status >= 300) {
                        throw TransportError("download HTTP " + std::to_string(status),
                                             status >= 500,
                                             static_cast<int>(status));
                    }
                    finish(op, {}, Json());
                    continue;
                }
                Json value;
                try {
                    value = Json::parse(op->response);
                } catch (const Json::exception&) {
                    if (status >= 500) {
                        throw TransportError(
                            "HTTP " + std::to_string(status), true, static_cast<int>(status));
                    }
                    throw ProtocolError("HTTP response is not valid JSON");
                }
                if ((status < 200 || status >= 300) &&
                    (!value.is_object() || !value.contains("ok") || !value.at("ok").is_boolean() ||
                     value.at("ok").get<bool>())) {
                    throw TransportError(
                        "HTTP " + std::to_string(status), status >= 500, static_cast<int>(status));
                }
                finish(op, {}, std::move(value));
            } catch (...) {
                finish(op, std::current_exception());
            }
        }
    }

    void run() noexcept {
        try {
            for (;;) {
                iteration();
                if (closing) {
                    std::lock_guard lock(mutex);
                    if (active.empty() && pending.empty()) {
                        return;
                    }
                }
                int events = 0;
                check_multi(curl_multi_poll(multi, nullptr, 0, 20, &events));
            }
        } catch (...) {
            closing.store(true);
            const auto error = std::current_exception();
            for (auto& [easy, op] : active) {
                curl_multi_remove_handle(multi, easy);
                finish(op, error);
            }
            active.clear();
            active_count = 0;
            std::deque<std::shared_ptr<Operation>> rest;
            {
                std::lock_guard lock(mutex);
                rest.swap(pending);
                queued_count = 0;
            }
            for (auto& op : rest) {
                finish(op, error);
            }
        }
    }
};

AsyncCurlTransport::AsyncCurlTransport(std::string token, AsyncCurlOptions options)
    : impl_(std::make_shared<Impl>(std::move(token), std::move(options))) {
    impl_->start();
}

AsyncCurlTransport::~AsyncCurlTransport() {
    request_stop();
}

void AsyncCurlTransport::request_stop() noexcept {
    impl_->stop();
}

AsyncTransportStats AsyncCurlTransport::stats() const {
    return {impl_->active_count.load(),
            impl_->queued_count.load(),
            impl_->completed_count.load(),
            impl_->failed_count.load()};
}

Task<Json> AsyncCurlTransport::perform(Request request) {
    if (!safe_name(request.method) || !request.parameters.is_object()) {
        throw std::invalid_argument("invalid API request");
    }
    auto executor = co_await asio::this_coro::executor;
    auto op = std::make_shared<Impl::Operation>(executor);
    op->request = std::move(request);
    op->owner = impl_;
    auto token = asio::use_awaitable;
    co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Json)>(
        [impl = impl_, op](auto handler) mutable {
            auto completion = std::make_shared<decltype(handler)>(std::move(handler));
            auto slot = asio::get_associated_cancellation_slot(*completion);
            if (slot.is_connected()) {
                slot.assign([weak = std::weak_ptr(op),
                             owner = std::weak_ptr(impl)](asio::cancellation_type) {
                    if (auto operation = weak.lock()) {
                        operation->cancelled = true;
                    }
                    if (auto state = owner.lock()) {
                        state->wake();
                    }
                });
            }
            op->complete = [completion, slot](std::exception_ptr error, Json result) mutable {
                if (slot.is_connected()) {
                    slot.clear();
                }
                (*completion)(error, std::move(result));
            };
            impl->submit(op);
        },
        token);
}

Task<void> AsyncCurlTransport::download(std::string file_path,
                                        std::shared_ptr<AsyncDownloadSink> sink,
                                        std::shared_ptr<RequestControl> control) {
    if (!sink || file_path.empty() || file_path.front() == '/' ||
        file_path.find("..") != std::string::npos ||
        file_path.find_first_of("%?#\\\r\n") != std::string::npos) {
        throw std::invalid_argument("invalid download path or sink");
    }
    auto executor = co_await asio::this_coro::executor;
    auto op = std::make_shared<Impl::Operation>(executor);
    op->sink = std::move(sink);
    op->download_path = std::move(file_path);
    op->request.control = std::move(control);
    op->owner = impl_;
    auto token = asio::use_awaitable;
    co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Json)>(
        [impl = impl_, op](auto handler) mutable {
            auto completion = std::make_shared<decltype(handler)>(std::move(handler));
            auto slot = asio::get_associated_cancellation_slot(*completion);
            if (slot.is_connected()) {
                slot.assign([weak = std::weak_ptr(op),
                             owner = std::weak_ptr(impl)](asio::cancellation_type) {
                    if (auto operation = weak.lock()) {
                        operation->cancelled = true;
                    }
                    if (auto state = owner.lock()) {
                        state->wake();
                    }
                });
            }
            op->complete = [completion, slot](std::exception_ptr error, Json result) mutable {
                if (slot.is_connected()) {
                    slot.clear();
                }
                (*completion)(error, std::move(result));
            };
            impl->submit(op);
        },
        token);
}
} // namespace tgpp
