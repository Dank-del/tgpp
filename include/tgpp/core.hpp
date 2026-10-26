#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tgpp {
using Json = nlohmann::json;

// Value semantics and indirection for recursive Telegram object graphs.
template <class T> class Box {
    std::shared_ptr<T> value_;

  public:
    Box() = default;

    Box(const T& value) : value_(std::make_shared<T>(value)) {}

    Box(T&& value) : value_(std::make_shared<T>(std::move(value))) {}

    Box(const Box& other) : value_(other ? std::make_shared<T>(*other) : nullptr) {}

    Box(Box&&) noexcept = default;

    Box& operator=(Box other) noexcept {
        value_.swap(other.value_);
        return *this;
    }

    explicit operator bool() const noexcept {
        return bool(value_);
    }

    const T& operator*() const {
        if (!value_) {
            throw std::logic_error("empty Box");
        }
        return *value_;
    }

    T& operator*() {
        if (!value_) {
            throw std::logic_error("empty Box");
        }
        return *value_;
    }

    const T* operator->() const {
        return &**this;
    }

    T* operator->() {
        return &**this;
    }
};

struct InputFile {
    std::string path;
    std::string filename;
    std::string content_type = "application/octet-stream";
};

struct Attachment {
    std::string name;
    InputFile file;
};

struct Request {
    std::string method;
    Json parameters = Json::object();
    std::vector<Attachment> files;

    // Extra named uploads support attach:// references inside media arrays.
    std::string attach(InputFile file) {
        std::size_t index = files.size();
        std::string name;
        bool exists;
        do {
            name = "upload_" + std::to_string(index++);
            exists = false;
            for (const auto& item : files) {
                if (item.name == name) {
                    exists = true;
                }
            }
        } while (exists);
        files.push_back({name, std::move(file)});
        return "attach://" + name;
    }
};

class Transport {
  public:
    virtual ~Transport() = default;
    // Return the complete Telegram response envelope, including "ok".
    virtual Json perform(const Request& request) = 0;
};

class ApiError : public std::runtime_error {
  public:
    int code;
    std::optional<std::int64_t> retry_after;
    std::optional<std::int64_t> migrate_to_chat_id;

    ApiError(int code,
             const std::string& description,
             std::optional<std::int64_t> retry_after = {},
             std::optional<std::int64_t> migrate_to_chat_id = {})
        : std::runtime_error(description), code(code), retry_after(retry_after),
          migrate_to_chat_id(migrate_to_chat_id) {}
};

class Client {
    std::shared_ptr<Transport> transport_;

  public:
    explicit Client(std::shared_ptr<Transport> transport) : transport_(std::move(transport)) {
        if (!transport_) {
            throw std::invalid_argument("transport must not be null");
        }
    }

    Json call(const Request& request) const;
};

Json encode_file(const InputFile& file, Request* request);
} // namespace tgpp
