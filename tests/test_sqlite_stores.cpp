#include "store_contract.hpp"
#include <filesystem>
#include <iostream>
#include <tgpp/ext/sqlite_stores.hpp>

namespace {
tgpp::Task<void> immediate_reads(std::shared_ptr<tgpp::ext::SqliteStores> store) {
    auto executor = co_await asio::this_coro::executor;
    for (int operation = 0; operation < 64; ++operation) {
        bool finished = false;
        for (int retry = 0; !finished; ++retry) {
            if (retry > 10000) {
                throw std::runtime_error("SQLite admission did not recover");
            }
            try {
                auto value = co_await store->checkpoint("no-checkpoint");
                store_contract::require(!value, "unexpected stress checkpoint");
                finished = true;
            } catch (const tgpp::OverloadError&) {
            }
            if (!finished) {
                asio::steady_timer wait(executor, std::chrono::milliseconds(1));
                co_await wait.async_wait(asio::use_awaitable);
            }
        }
    }
}

tgpp::Task<void> release_after_completion(std::shared_ptr<tgpp::ext::SqliteStores> store) {
    co_await store->checkpoint("immediate-release");
    // The DB worker may still be returning from its completion post on another
    // thread. Dropping the public store must never self-join that worker's pool.
    store.reset();
}

void completion_races() {
    tgpp::Runtime runtime({4, 1});
    auto store = std::make_shared<tgpp::ext::SqliteStores>(":memory:", tgpp::ext::StoreClock{}, 4);
    std::vector<std::future<void>> futures;
    for (int worker = 0; worker < 32; ++worker) {
        futures.push_back(
            asio::co_spawn(runtime.executor(), immediate_reads(store), asio::use_future));
    }
    for (auto& future : futures) {
        future.get();
    }
    store_contract::require(
        !asio::co_spawn(runtime.executor(), store->checkpoint("after-stress"), asio::use_future)
             .get(),
        "SQLite pending permits did not recover");
    store.reset();
    futures.clear();
    for (int iteration = 0; iteration < 32; ++iteration) {
        futures.push_back(asio::co_spawn(
            runtime.executor(),
            release_after_completion(std::make_shared<tgpp::ext::SqliteStores>(":memory:")),
            asio::use_future));
    }
    for (auto& future : futures) {
        future.get();
    }
}
} // namespace

int main() {
    auto path = std::filesystem::temp_directory_path() /
                ("tgpp-store-" + std::to_string(tgpp::ext::store_time_ms()) + ".sqlite");
    try {
        std::int64_t now = 1000;
        {
            tgpp::ext::SqliteStores stores(path.string(), [&] { return now; });
            store_contract::verify(stores, stores, now);
            store_contract::verify_inbox(stores, now);
            store_contract::verify_competing(stores, stores);
            store_contract::verify_session_fence(stores, stores, now);
            std::vector<tgpp::ext::InboxUpdate> batch{{20, "key", tgpp::Json{{"update_id", 20}}}};
            store_contract::run(stores.enqueue("restart", batch, 21));
            auto claim =
                store_contract::run(stores.claim("restart", "old", std::chrono::milliseconds(100)));
            store_contract::require(claim.has_value(), "claim before restart");
        }
        now += 101;
        {
            tgpp::ext::SqliteStores stores(path.string(), [&] { return now; });
            store_contract::require(store_contract::run(stores.checkpoint("restart")) == 21,
                                    "checkpoint survives restart");
            auto claim =
                store_contract::run(stores.claim("restart", "new", std::chrono::milliseconds(100)));
            store_contract::require(claim && claim->attempts == 2,
                                    "expired lease survives restart");
            store_contract::require(store_contract::run(stores.complete(*claim)),
                                    "complete after restart");
            auto record = store_contract::run(stores.load({"bot", "chat"}));
            store_contract::require(record && record->value.at("step") == 2,
                                    "session survives restart");
        }
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + "-wal");
        std::filesystem::remove(path.string() + "-shm");
        completion_races();
        std::cout << "SQLite store contracts, restart and completion race checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove(path);
        return 1;
    }
}
