#pragma once

#include "core.hpp"
#include <limits>
#include <type_traits>

namespace tgpp {
namespace detail {
// Keep small decoders allocation-free while large generated aggregate
// scratch values use heap storage on platforms with 512-KiB worker stacks.
template <class T, bool Large = (sizeof(T) > 8 * 1024)> class DecodeStorage {
    T value_{};

  public:
    T& value() noexcept {
        return value_;
    }
};

template <class T> class DecodeStorage<T, true> {
    std::unique_ptr<T> value_ = std::make_unique<T>();

  public:
    T& value() noexcept {
        return *value_;
    }
};
} // namespace detail

template <class T, class = void> struct Codec {
    static Json encode(const T& value, Request* request) {
        return encode_object(value, request);
    }

    static void decode(const Json& json, T& value) {
        decode_object(json, value);
    }
};

template <class T> struct Codec<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
    static Json encode(T value, Request*) {
        return value;
    }

    static void decode(const Json& json, T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            if (!json.is_boolean()) {
                throw std::invalid_argument("expected Boolean");
            }
        } else if constexpr (std::is_integral_v<T>) {
            if (!json.is_number_integer()) {
                throw std::invalid_argument("expected Integer");
            }
            if (json.is_number_unsigned() &&
                json.get<std::uint64_t>() > std::uint64_t(std::numeric_limits<T>::max())) {
                throw std::out_of_range("Integer exceeds int64 range");
            }
        } else {
            if (!json.is_number()) {
                throw std::invalid_argument("expected Float");
            }
        }
        json.get_to(value);
    }
};

template <> struct Codec<std::string> {
    static Json encode(const std::string& value, Request*) {
        return value;
    }

    static void decode(const Json& json, std::string& value) {
        if (!json.is_string()) {
            throw std::invalid_argument("expected String");
        }
        json.get_to(value);
    }
};

template <> struct Codec<InputFile> {
    static Json encode(const InputFile& value, Request* request) {
        return encode_file(value, request);
    }

    static void decode(const Json&, InputFile&) {
        throw std::invalid_argument("InputFile is upload-only");
    }
};

template <class T> struct Codec<Box<T>> {
    static Json encode(const Box<T>& value, Request* request) {
        if (!value) {
            throw std::invalid_argument("required recursive object is empty");
        }
        return Codec<T>::encode(*value, request);
    }

    static void decode(const Json& json, Box<T>& value) {
        detail::DecodeStorage<T> item;
        Codec<T>::decode(json, item.value());
        value = Box<T>(std::move(item.value()));
    }
};

template <class T> struct Codec<std::optional<T>> {
    static Json encode(const std::optional<T>& value, Request* request) {
        return value ? Codec<T>::encode(*value, request) : Json(nullptr);
    }

    static void decode(const Json& json, std::optional<T>& value) {
        if (json.is_null()) {
            value.reset();
            return;
        }
        detail::DecodeStorage<T> item;
        Codec<T>::decode(json, item.value());
        value = std::move(item.value());
    }
};

template <class T> struct Codec<std::vector<T>> {
    static Json encode(const std::vector<T>& value, Request* request) {
        Json json = Json::array();
        for (const auto& item : value) {
            json.push_back(Codec<T>::encode(item, request));
        }
        return json;
    }

    static void decode(const Json& json, std::vector<T>& value) {
        if (!json.is_array()) {
            throw std::invalid_argument("expected Array");
        }
        std::vector<T> items;
        items.reserve(json.size());
        for (const auto& entry : json) {
            if constexpr (std::is_same_v<T, bool>) {
                // vector<bool> exposes a proxy rather than a bool reference.
                bool item{};
                Codec<bool>::decode(entry, item);
                items.push_back(item);
            } else {
                // Large generated values (Update is >230 KiB) must be decoded
                // directly in vector-owned heap storage on small thread stacks.
                items.emplace_back();
                Codec<T>::decode(entry, items.back());
            }
        }
        value = std::move(items);
    }
};

template <class... Ts> struct Codec<std::variant<Ts...>> {
    using Variant = std::variant<Ts...>;

    static Json encode(const Variant& value, Request* request) {
        return std::visit(
            [request](const auto& item) {
                return Codec<std::decay_t<decltype(item)>>::encode(item, request);
            },
            value);
    }

    template <std::size_t I = 0> static void choose(const Json& json, Variant& value) {
        if constexpr (I == sizeof...(Ts)) {
            throw std::invalid_argument("JSON does not match any union alternative");
        } else {
            using T = std::variant_alternative_t<I, Variant>;
            std::optional<detail::DecodeStorage<T>> candidate;
            try {
                candidate.emplace();
                Codec<T>::decode(json, candidate->value());
            } catch (const std::invalid_argument&) {
                candidate.reset();
            } catch (const nlohmann::json::exception&) {
                candidate.reset();
            }
            if (candidate) {
                value.template emplace<I>(std::move(candidate->value()));
            } else {
                choose<I + 1>(json, value);
            }
        }
    }

    static void decode(const Json& json, Variant& value) {
        choose(json, value);
    }
};

template <class T> Json to_json(const T& value, Request* request = nullptr) {
    return Codec<T>::encode(value, request);
}

template <class T> T from_json(const Json& json) {
    T value{};
    Codec<T>::decode(json, value);
    return value;
}

namespace detail {
template <class T>
void write_field(Json& json, const char* name, const T& value, Request* request) {
    json[name] = Codec<T>::encode(value, request);
}

template <class T>
void write_field(Json& json, const char* name, const std::optional<T>& value, Request* request) {
    if (value) {
        json[name] = Codec<T>::encode(*value, request);
    }
}

template <class T> void read_required(const Json& json, const char* name, T& value) {
    Codec<T>::decode(json.at(name), value);
}

template <class T> void read_optional(const Json& json, const char* name, std::optional<T>& value) {
    auto item = json.find(name);
    if (item == json.end() || item->is_null()) {
        value.reset();
    } else {
        Codec<std::optional<T>>::decode(*item, value);
    }
}
} // namespace detail
} // namespace tgpp
