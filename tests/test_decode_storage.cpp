#include <array>
#include <iostream>
#include <tgpp/types.hpp>
using namespace tgpp;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

struct Small {
    inline static int alive = 0, allocations = 0, deletions = 0;
    int value = 0;

    Small() {
        ++alive;
    }

    ~Small() {
        --alive;
    }

    static void* operator new(std::size_t size) {
        ++allocations;
        return ::operator new(size);
    }

    static void operator delete(void* pointer) noexcept {
        ++deletions;
        ::operator delete(pointer);
    }
};

struct Large {
    inline static int alive = 0, allocations = 0, deletions = 0;
    std::array<char, 9000> data{};

    Large() {
        ++alive;
    }

    ~Large() {
        --alive;
    }

    static void* operator new(std::size_t size) {
        ++allocations;
        return ::operator new(size);
    }

    static void operator delete(void* pointer) noexcept {
        ++deletions;
        ::operator delete(pointer);
    }
};

struct ThrowSmall : Small {
    ThrowSmall() {
        throw std::runtime_error("constructor");
    }
};

struct ThrowLarge : Large {
    ThrowLarge() {
        throw std::runtime_error("constructor");
    }
};

static_assert(sizeof(detail::DecodeStorage<Large>) < sizeof(Large));

void storage_lifetime() {
    {
        detail::DecodeStorage<Small> value;
        require(Small::alive == 1 && Small::allocations == 0,
                "small decode scratch allocated or failed construction");
        value.value().value = 7;
        require(value.value().value == 7, "small scratch did not retain value");
    }
    require(Small::alive == 0 && Small::deletions == 0, "small scratch destructor mismatch");
    {
        detail::DecodeStorage<Large> value;
        require(Large::alive == 1 && Large::allocations == 1,
                "large decode scratch was not heap allocated once");
        value.value().data[0] = 'x';
        require(value.value().data[0] == 'x', "large scratch did not retain value");
    }
    require(Large::alive == 0 && Large::deletions == 1, "large scratch destructor mismatch");
    try {
        detail::DecodeStorage<ThrowSmall> value;
        throw std::logic_error("missing construction error");
    } catch (const std::runtime_error&) {
    }
    try {
        detail::DecodeStorage<ThrowLarge> value;
        throw std::logic_error("missing construction error");
    } catch (const std::runtime_error&) {
    }
    require(Small::alive == 0 && Small::allocations == 0 && Large::alive == 0 &&
                Large::allocations == Large::deletions,
            "throwing scratch construction leaked live objects or storage");
}

void raw_update_decode() {
    Json payload{{"update_id", 42},
                 {"message",
                  {{"message_id", 1},
                   {"date", 1},
                   {"text", "hello"},
                   {"chat", {{"id", 9}, {"type", "private"}}}}}};
    auto update = from_json<Update>(payload);
    require(update.update_id == 42 && update.message && update.message->text == "hello",
            "raw Update decode failed");
    bool rejected = false;
    try {
        Codec<Update>::decode(Json{{"update_id", "invalid"}}, update);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && update.update_id == 42 && update.message && update.message->text == "hello",
            "failed decode changed original Update");
}
} // namespace

int main() {
    try {
        storage_lifetime();
        raw_update_decode();
        std::cout << "decode scratch storage and raw Update passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
