#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <curl/curl.h>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <tgpp/curl_transport.hpp>

namespace tgpp {
namespace {
struct GlobalCurl {
    GlobalCurl() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("libcurl initialization failed");
        }
    }

    ~GlobalCurl() {
        curl_global_cleanup();
    }
};

bool safe_name(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    });
}

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* target) noexcept {
    if (size && count > std::numeric_limits<std::size_t>::max() / size) {
        return 0;
    }
    const auto length = size * count;
    auto& body = *static_cast<std::string*>(target);
    if (length > 64 * 1024 * 1024 - body.size()) {
        return 0;
    }
    try {
        body.append(data, length);
    } catch (...) {
        return 0;
    }
    return length;
}

void checked(CURLcode result) {
    // Never include the URL: it contains the bot token.
    if (result != CURLE_OK) {
        const bool retryable = result == CURLE_COULDNT_RESOLVE_HOST ||
                               result == CURLE_COULDNT_RESOLVE_PROXY ||
                               result == CURLE_COULDNT_CONNECT ||
                               result == CURLE_OPERATION_TIMEDOUT || result == CURLE_SEND_ERROR ||
                               result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING ||
                               result == CURLE_PARTIAL_FILE || result == CURLE_SSL_CONNECT_ERROR;
        throw TransportError(std::string("HTTP transport: ") + curl_easy_strerror(result),
                             retryable,
                             static_cast<int>(result));
    }
}
} // namespace

struct CurlTransport::Pool {
    std::mutex mutex;
    std::condition_variable available;
    std::vector<CURL*> idle;
    std::size_t created = 0, capacity;
    long timeout;

    Pool(std::size_t capacity, long timeout) : capacity(capacity), timeout(timeout) {
        idle.reserve(capacity);
    }

    ~Pool() {
        for (auto* handle : idle) {
            curl_easy_cleanup(handle);
        }
    }

    struct Release {
        Pool* pool;

        void operator()(CURL* handle) const noexcept {
            // Options contain pointers into request-local data. Reset before
            // returning the handle; live connections and caches are retained.
            curl_easy_reset(handle);
            {
                std::lock_guard<std::mutex> lock(pool->mutex);
                pool->idle.push_back(handle);
            }
            pool->available.notify_one();
        }
    };

    using Lease = std::unique_ptr<CURL, Release>;

    Lease acquire() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!available.wait_for(lock, std::chrono::seconds(timeout), [&] {
                return !idle.empty() || created < capacity;
            })) {
            throw std::runtime_error("HTTP connection pool wait timed out");
        }
        if (!idle.empty()) {
            auto* handle = idle.back();
            idle.pop_back();
            return Lease(handle, Release{this});
        }
        auto* handle = curl_easy_init();
        if (!handle) {
            throw std::runtime_error("could not allocate curl handle");
        }
        ++created;
        return Lease(handle, Release{this});
    }
};

CurlTransport::CurlTransport(std::string token, CurlOptions options)
    : token_(std::move(token)), options_(std::move(options)) {
    static GlobalCurl global;
    if (token_.empty() ||
        token_.find_first_not_of(
            "0123456789:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") !=
            std::string::npos) {
        throw std::invalid_argument("invalid bot token characters");
    }
    while (!options_.base_url.empty() && options_.base_url.back() == '/') {
        options_.base_url.pop_back();
    }
    if (options_.base_url.rfind("https://", 0) != 0 && options_.base_url.rfind("http://", 0) != 0) {
        throw std::invalid_argument("base URL must use http:// or https://");
    }
    if (options_.connect_timeout_seconds <= 0 || options_.timeout_seconds <= 0) {
        throw std::invalid_argument("HTTP timeouts must be positive");
    }
    if (!options_.max_connections || options_.pool_timeout_seconds <= 0) {
        throw std::invalid_argument("HTTP pool capacity and timeout must be positive");
    }
    pool_ = std::make_shared<Pool>(options_.max_connections, options_.pool_timeout_seconds);
}

Json CurlTransport::perform(const Request& request) {
    if (!safe_name(request.method)) {
        throw std::invalid_argument("invalid API method name");
    }
    if (!request.parameters.is_object()) {
        throw std::invalid_argument("request parameters must be an object");
    }
    auto handle = pool_->acquire();
    std::string response;
    const auto url = options_.base_url + "/bot" + token_ + "/" + request.method;
    checked(curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str()));
    checked(curl_easy_setopt(handle.get(), CURLOPT_POST, 1L));
    checked(curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L));
    checked(
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, options_.connect_timeout_seconds));
    checked(curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, options_.timeout_seconds));
    checked(curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_body));
    checked(curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response));
    // Redirects stay disabled; TLS peer and hostname verification retain curl defaults.
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(nullptr, curl_mime_free);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr,
                                                                        curl_slist_free_all);
    std::string body;
    if (request.files.empty()) {
        body = request.parameters.dump();
        headers.reset(curl_slist_append(nullptr, "Content-Type: application/json"));
        if (!headers) {
            throw std::bad_alloc();
        }
        checked(curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get()));
        checked(curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.c_str()));
        checked(curl_easy_setopt(
            handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())));
    } else {
        mime.reset(curl_mime_init(handle.get()));
        if (!mime) {
            throw std::bad_alloc();
        }
        std::set<std::string> names;
        for (auto it = request.parameters.begin(); it != request.parameters.end(); ++it) {
            names.insert(it.key());
            auto* part = curl_mime_addpart(mime.get());
            if (!part) {
                throw std::bad_alloc();
            }
            checked(curl_mime_name(part, it.key().c_str()));
            auto value = it->is_string() ? it->get<std::string>() : it->dump();
            checked(curl_mime_data(part, value.c_str(), value.size()));
        }
        for (const auto& attachment : request.files) {
            if (!safe_name(attachment.name) || !names.insert(attachment.name).second) {
                throw std::invalid_argument("invalid or duplicate attachment name");
            }
            if (attachment.file.source) {
                throw std::invalid_argument("async upload sources require AsyncCurlTransport");
            }
            if (attachment.file.path.empty() && !attachment.file.memory) {
                throw std::invalid_argument("upload path must not be empty");
            }
            auto* part = curl_mime_addpart(mime.get());
            if (!part) {
                throw std::bad_alloc();
            }
            checked(curl_mime_name(part, attachment.name.c_str()));
            if (attachment.file.memory) {
                checked(
                    curl_mime_data(part,
                                   reinterpret_cast<const char*>(attachment.file.memory->data()),
                                   attachment.file.memory->size()));
            } else {
                checked(curl_mime_filedata(part, attachment.file.path.c_str()));
            }
            const auto filename =
                attachment.file.filename.empty()
                    ? std::filesystem::path(attachment.file.path).filename().string()
                    : attachment.file.filename;
            checked(curl_mime_filename(part, filename.c_str()));
            checked(curl_mime_type(part, attachment.file.content_type.c_str()));
        }
        checked(curl_easy_setopt(handle.get(), CURLOPT_MIMEPOST, mime.get()));
    }
    checked(curl_easy_perform(handle.get()));
    long status = 0;
    checked(curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status));
    Json json;
    try {
        json = Json::parse(response);
    } catch (const Json::exception&) {
        if (status >= 500) {
            throw TransportError("HTTP " + std::to_string(status), true, static_cast<int>(status));
        }
        throw ProtocolError("HTTP " + std::to_string(status) + ": invalid JSON response");
    }
    if (status < 200 || status >= 300) {
        // Telegram returns actionable JSON on 400/401/429. Preserve it for Client.
        if (!json.is_object() || !json.contains("ok") || !json.at("ok").is_boolean() ||
            json.at("ok").get<bool>()) {
            throw TransportError(
                "HTTP " + std::to_string(status), status >= 500, static_cast<int>(status));
        }
    }
    return json;
}
} // namespace tgpp
