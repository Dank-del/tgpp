#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <iostream>
#include <tgpp/ext/stores.hpp>

// Replace MemoryStores with SqliteStores or an injected remote SessionStore.
tgpp::Task<void> example(tgpp::ext::SessionStore& store) {
    tgpp::ext::SessionKey key{"example-bot", "chat:42"};
    auto current = co_await store.load(key);
    auto revision = co_await store.compare_exchange(
        key, current ? current->version : 0, tgpp::Json{{"step", "waiting_for_name"}});
    if (!revision) {
        throw std::runtime_error("concurrent session change");
    }
    std::cout << "Saved session revision " << *revision << '\n';
}

int main() {
    asio::io_context io;
    tgpp::ext::MemoryStores store;
    auto done = asio::co_spawn(io, example(store), asio::use_future);
    io.run();
    done.get();
}
