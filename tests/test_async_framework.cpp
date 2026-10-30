#include <asio.hpp>
#include <atomic>
#include <barrier>
#include <future>
#include <iostream>
#include <mutex>
#include <tgpp/ext/async.hpp>
#if defined(__GLIBC__) && defined(__linux__)
#include <pthread.h>
#endif
using namespace tgpp;
using namespace tgpp::ext;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Mock final : public AsyncTransport {
  public:
    std::atomic<int> calls{0};

    Task<Json> perform(Request request) override {
        ++calls;
        if (request.method == "getUpdates") {
            if (calls == 1) {
                throw TransportError("temporary test error", true);
            }
            co_return Json{{"ok", true}, {"result", Json::array()}};
        }
        co_return Json{{"ok", true}, {"result", true}};
    }
};

Update update(int id, int chat) {
    Update u{};
    u.update_id = id;
    Message m{};
    m.message_id = id;
    m.date = 1;
    m.chat.id = chat;
    m.chat.type = "private";
    m.text = "hello";
    u.message = std::move(m);
    return u;
}

Task<void> delay(std::chrono::milliseconds duration) {
    asio::steady_timer timer(co_await asio::this_coro::executor, duration);
    co_await timer.async_wait(asio::use_awaitable);
}

void ordering() {
    Runtime runtime({1, 2});
    auto transport = std::make_shared<Mock>();
    AsyncApplicationOptions options;
    options.max_active_keys = 2;
    AsyncApplication app(runtime.executor(), transport, options);
    std::map<std::int64_t, int> active, last;
    int simultaneous = 0, peak = 0;
    app.add(
        "ordered",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext& c) -> Task<void> {
            auto key = c.chat()->id;
            require(active[key]++ == 0, "same-key handlers overlapped");
            require(c.update().update_id > last[key], "same-key dispatch reordered");
            last[key] = int(c.update().update_id);
            peak = std::max(peak, ++simultaneous);
            co_await delay(std::chrono::milliseconds(2));
            require(c.message()->text == "hello", "context lifetime lost");
            --active[key];
            --simultaneous;
        });
    std::vector<std::future<Result>> work;
    for (int id = 1; id <= 18; ++id) {
        work.push_back(
            asio::co_spawn(runtime.executor(), app.process(update(id, id % 3)), asio::use_future));
    }
    for (auto& future : work) {
        future.get();
    }
    require(peak == 2, "independent keys did not progress concurrently");
    auto stats = asio::co_spawn(runtime.executor(), app.stats(), asio::use_future).get();
    require(stats.processed == 18 && stats.queued == 0, "invalid completion counters");
    asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
    bool cancelled = false;
    try {
        asio::co_spawn(runtime.executor(), app.process(update(99, 1)), asio::use_future).get();
    } catch (const CancelledError&) {
        cancelled = true;
    }
    require(cancelled, "closed application accepted new work");
}

void failures() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncApplication app(runtime.executor(), transport);
    int errors = 0;
    app.add(
        "bad",
        [](const AsyncContext&) { return true; },
        [](AsyncContext&) -> Task<void> {
            throw std::runtime_error("handler");
            co_return;
        });
    app.on_error([&](AsyncContext&, std::exception_ptr) -> Task<ErrorAction> {
        ++errors;
        co_return ErrorAction::skip;
    });
    require(asio::co_spawn(runtime.executor(), app.process(update(1, 1)), asio::use_future).get() ==
                Result::handled,
            "skip policy failed");
    require(errors == 1, "missing error hook");
}

void retry_and_checkpoint() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncApplication app(runtime.executor(), transport);
    AsyncPollingOptions options;
    options.polling.retry.initial_delay = std::chrono::milliseconds(1);
    options.polling.retry.max_delay = std::chrono::milliseconds(1);
    int retries = 0;
    options.polling.retry.on_retry = [&](std::exception_ptr, std::chrono::milliseconds) {
        ++retries;
    };
    AsyncPolling polling(app, options);
    require(asio::co_spawn(runtime.executor(), polling.poll_once(), asio::use_future).get() == 0,
            "unexpected updates");
    require(transport->calls == 2 && retries == 1, "transient retrieval was not retried");
    auto stores = std::make_shared<MemoryStores>();
    app.use(sessions(stores, "test"));
    app.add(
        "session",
        [](const AsyncContext&) { return true; },
        [](AsyncContext& c) -> Task<void> {
            c.get<SessionData>().value["count"] = 1;
            co_return;
        });
    asio::co_spawn(runtime.executor(), app.process(update(1, 3)), asio::use_future).get();
    auto stored =
        asio::co_spawn(runtime.executor(), stores->load({"test", "3"}), asio::use_future).get();
    require(stored && stored->value["count"] == 1, "session did not persist");
}

void immediate_polling_completion_race() {
    struct Immediate final : AsyncTransport {
        std::barrier<> release{2};
        int calls = 0;

        Task<Json> perform(Request request) override {
            require(request.method == "getUpdates", "unexpected polling method");
            release.arrive_and_wait();
            auto item = update(++calls, 77);
            co_return Json{{"ok", true}, {"result", Json::array({to_json(item)})}};
        }
    };

    Runtime runtime({2, 1});
    auto transport = std::make_shared<Immediate>();
    AsyncApplication app(runtime.executor(), transport);
    std::atomic<int> handled{0};
    app.add(
        "immediate",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext&) -> Task<void> {
            ++handled;
            co_return;
        });
    AsyncPolling polling(app);
    // Release retrieval from the controller while both executor threads can
    // submit/complete dispatch. An immediate completion must never invalidate
    // the still-returning async initiation frame or its loop captures.
    for (int round = 0; round < 256; ++round) {
        auto done = asio::co_spawn(runtime.executor(), polling.poll_once(), asio::use_future);
        transport->release.arrive_and_wait();
        require(done.get() == 1 && handled == round + 1, "immediate update lost or repeated");
        require(polling.offset() == round + 2, "immediate completion lost checkpoint");
    }
    asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
}

void sync_retry_is_retrieval_only() {
    struct TransportMock final : Transport {
        int calls = 0;

        Json perform(const Request&) override {
            ++calls;
            if (calls == 1) {
                throw TransportError("temporary", true);
            }
            return Json{{"ok", true}, {"result", Json::array({to_json(update(1, 1))})}};
        }
    };

    auto transport = std::make_shared<TransportMock>();
    Application app(transport);
    app.router().add<FunctionHandler>(
        "failure", filters::all(), [](Context&) { throw TransportError("handler failure", true); });
    PollingOptions options;
    options.retry.initial_delay = std::chrono::milliseconds(1);
    options.retry.max_delay = std::chrono::milliseconds(1);
    int retries = 0;
    options.retry.on_retry = [&](std::exception_ptr, std::chrono::milliseconds) { ++retries; };
    Polling polling(app, options);
    bool failed = false;
    try {
        polling.poll_once();
    } catch (const TransportError&) {
        failed = true;
    }
    require(failed && transport->calls == 2 && retries == 1,
            "handler error incorrectly retried as retrieval failure");
    require(!polling.offset(), "failed handler acknowledged update");
}

void routing_and_continuation() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncApplication app(runtime.executor(), transport);
    std::vector<int> order;
    auto child = std::make_shared<AsyncRouter>("child");
    child->use([&](AsyncContext&, AsyncNext next) -> Task<Result> {
        order.push_back(1);
        auto result = co_await next();
        order.push_back(3);
        co_return result;
    });
    child->add(
        "handler",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext&) -> Task<void> {
            order.push_back(2);
            co_return;
        });
    app.include(child);
    app.add(
        "fallback",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext&) -> Task<void> {
            order.push_back(4);
            co_return;
        });
    asio::co_spawn(runtime.executor(), app.process(update(1, 1)), asio::use_future).get();
    require(order == std::vector<int>({1, 2, 3}), "nested middleware/route order changed");
}

void grace_cancellation() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncApplication app(runtime.executor(), transport);
    std::atomic<bool> entered = false;
    app.add(
        "slow",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext&) -> Task<void> {
            entered = true;
            co_await delay(std::chrono::seconds(20));
        });
    auto handler = asio::co_spawn(runtime.executor(), app.process(update(1, 1)), asio::use_future);
    while (!entered) {
        std::this_thread::yield();
    }
    bool timedout = false;
    try {
        asio::co_spawn(
            runtime.executor(), app.shutdown(std::chrono::milliseconds(1)), asio::use_future)
            .get();
    } catch (const TimeoutError&) {
        timedout = true;
    }
    require(timedout, "grace did not expire");
    require(handler.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "cancelled handler did not complete");
    try {
        handler.get();
    } catch (const std::exception&) {
    }
    asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
}

void batch_checkpoint_prefix() {
    class BatchTransport final : public AsyncTransport {
      public:
        int calls = 0;

        Task<Json> perform(Request) override {
            ++calls;
            Json batch = Json::array();
            for (int id = 1; id <= 3; ++id) {
                auto item = std::make_unique<Update>();
                item->update_id = id;
                item->message.emplace();
                auto& message = *item->message;
                message.message_id = id;
                message.date = 1;
                message.chat.id = id;
                message.chat.type = "private";
                message.text = "hello";
                batch.push_back(to_json(*item));
            }
            co_return Json{{"ok", true}, {"result", std::move(batch)}};
        }
    };

    Runtime runtime;
    auto transport = std::make_shared<BatchTransport>();
    AsyncApplication app(runtime.executor(), transport);
    std::atomic<int> processed = 0;
    app.add(
        "batch",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext& c) -> Task<void> {
            if (c.update().update_id == 2) {
                throw TransportError("handler failed", true);
            }
            co_await delay(std::chrono::milliseconds(1));
            ++processed;
        });
    AsyncPolling polling(app);
    bool failed = false;
    try {
        asio::co_spawn(runtime.executor(), polling.poll_once(), asio::use_future).get();
    } catch (const TransportError&) {
        failed = true;
    }
    require(failed && polling.offset() == 2 && processed == 2 && transport->calls == 1,
            "polling did not retain successful-prefix checkpoint");
}

void overload_bound() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    AsyncApplicationOptions options;
    options.max_active_keys = 1;
    options.queue_capacity = 1;
    AsyncApplication app(runtime.executor(), transport, options);
    std::atomic<bool> entered = false;
    app.add(
        "wait",
        [](const AsyncContext&) { return true; },
        [&](AsyncContext&) -> Task<void> {
            entered = true;
            co_await delay(std::chrono::seconds(20));
        });
    auto first = asio::co_spawn(runtime.executor(), app.process(update(1, 1)), asio::use_future);
    while (!entered) {
        std::this_thread::yield();
    }
    auto second = asio::co_spawn(runtime.executor(), app.process(update(2, 2)), asio::use_future);
    while (asio::co_spawn(runtime.executor(), app.stats(), asio::use_future).get().queued != 1) {
        std::this_thread::yield();
    }
    bool overloaded = false;
    try {
        asio::co_spawn(runtime.executor(), app.process(update(3, 3)), asio::use_future).get();
    } catch (const OverloadError&) {
        overloaded = true;
    }
    require(overloaded, "bounded queue admitted excess update");
    try {
        asio::co_spawn(
            runtime.executor(), app.shutdown(std::chrono::milliseconds(1)), asio::use_future)
            .get();
    } catch (const TimeoutError&) {
    }
    for (auto* future : {&first, &second}) {
        require(future->wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "shutdown left pending dispatch");
        try {
            future->get();
        } catch (const std::exception&) {
        }
    }
    asio::co_spawn(runtime.executor(), app.shutdown(), asio::use_future).get();
}

void durable_session_completion() {
    Runtime runtime;
    auto transport = std::make_shared<Mock>();
    auto store = std::make_shared<MemoryStores>();
    AsyncApplication app(runtime.executor(), transport);
    app.use(sessions(store, "durable"));
    app.add(
        "persist",
        [](const AsyncContext&) { return true; },
        [](AsyncContext& c) -> Task<void> {
            require(c.find<UpdateClaim>() != nullptr, "durable claim metadata missing");
            c.get<SessionData>().value["done"] = true;
            co_return;
        });
    std::vector<InboxUpdate> items;
    items.push_back({1, "3", to_json(update(1, 3))});
    asio::co_spawn(
        runtime.executor(), store->enqueue("durable", std::move(items), 2), asio::use_future)
        .get();
    AsyncPollingOptions options;
    options.store = store;
    options.bot_identity = "durable";
    options.lease = std::chrono::milliseconds(90);
    AsyncPolling polling(app, options);
    require(asio::co_spawn(runtime.executor(), polling.process_pending(), asio::use_future).get(),
            "durable worker did not consume update");
    require(!asio::co_spawn(runtime.executor(), polling.process_pending(), asio::use_future).get(),
            "completed durable update replayed");
    auto saved =
        asio::co_spawn(runtime.executor(), store->load({"durable", "3"}), asio::use_future).get();
    require(saved && saved->value["done"] == true, "fenced durable session did not commit");
}
} // namespace

int main() {
    try {
#if defined(__GLIBC__) && defined(__linux__)
        // macOS workers have 512-KiB stacks. Exercise that limit on Linux too,
        // while preserving the main thread's normal stack for fixture setup.
        pthread_attr_t attributes;
        require(pthread_attr_init(&attributes) == 0, "could not initialize worker attributes");
        const int sized = pthread_attr_setstacksize(&attributes, 512 * 1024);
        const int configured = sized ? 0 : pthread_setattr_default_np(&attributes);
        const int destroyed = pthread_attr_destroy(&attributes);
        require(sized == 0 && configured == 0 && destroyed == 0,
                "could not configure 512-KiB regression worker stacks");
#endif
        ordering();
        failures();
        retry_and_checkpoint();
        sync_retry_is_retrieval_only();
        immediate_polling_completion_race();
        routing_and_continuation();
        grace_cancellation();
        batch_checkpoint_prefix();
        overload_bound();
        durable_session_completion();
        std::cout << "async framework tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
