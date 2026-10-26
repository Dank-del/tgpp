#include <tgpp/ext/ext.hpp>
#include <atomic>
#include <iostream>

using namespace tgpp;
using namespace tgpp::ext;
using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Dummy : Transport {
    std::function<Json()> response;
    Json perform(const Request&) override { return response ? response() : Json{{"ok", true}, {"result", true}}; }
};
Update update(std::int64_t id, std::int64_t chat) {
    Update value{}; value.update_id = id;
    Message message{}; message.message_id = id; message.chat.id = chat; message.chat.type = "private"; message.text = "payload";
    value.message = std::move(message); return value;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    int entered = 0;
    bool released = false;
    void enter() {
        std::unique_lock<std::mutex> lock(mutex); ++entered; changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    bool wait(int count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return entered >= count; });
    }
    void release() { { std::lock_guard<std::mutex> lock(mutex); released = true; } changed.notify_all(); }
};
struct ReleaseGate { Gate& gate; ~ReleaseGate() { gate.release(); } };
ConcurrentApplication::Factory factory(std::shared_ptr<Transport> transport, Callback callback) {
    return [transport = std::move(transport), callback = std::move(callback)] {
        auto app = std::make_unique<Application>(transport);
        app->router().add<FunctionHandler>("work", filters::all(), callback);
        return app;
    };
}
void parallel_chats_and_ordering() {
    auto transport = std::make_shared<Dummy>(); Gate gate;
    ConcurrentApplication pool(factory(transport, [&](Context&) { gate.enter(); }), {2, 8, {}});
    auto first = pool.submit(update(1, 1)); auto second = pool.submit(update(2, 2));
    const bool overlapped = gate.wait(2); gate.release();
    require(first.get() == Result::handled && second.get() == Result::handled, "concurrent handler results");
    pool.shutdown(); require(overlapped, "different chats must execute concurrently");

    std::vector<std::int64_t> order;
    ConcurrentApplication ordered(factory(transport, [&](Context& context) { order.push_back(context.update().update_id); }), {3, 8, {}});
    std::vector<std::future<Result>> futures;
    for (int i = 1; i <= 100; ++i) futures.push_back(ordered.submit(update(i, 7)));
    for (auto& future : futures) future.get(); ordered.shutdown();
    for (int i = 0; i < 100; ++i) require(order[i] == i + 1, "same chat must remain FIFO");
}
void backpressure_and_draining() {
    auto transport = std::make_shared<Dummy>(); Gate gate;
    ConcurrentApplication pool(factory(transport, [&](Context&) { gate.enter(); }), {1, 1, {}});
    ReleaseGate release{gate};
    auto active = pool.submit(update(1, 1));
    const bool entered = gate.wait(1);
    if (!entered) { gate.release(); pool.shutdown(); throw std::runtime_error("worker did not enter handler"); }
    auto queued = pool.submit(update(2, 1));
    auto rejected = update(3, 1);
    require(!pool.try_submit(rejected) && rejected.message->text == "payload", "full queue must reject without consuming input");
    auto blocked = std::async(std::launch::async, [&] {
        try { pool.submit(update(4, 1)); return false; } catch (const std::logic_error&) { return true; }
    });
    pool.request_stop(); gate.release();
    require(blocked.get(), "shutdown must release blocked producers");
    require(active.get() == Result::handled && queued.get() == Result::handled, "accepted jobs must drain on shutdown");
    pool.shutdown();
    require(!pool.try_submit(rejected), "stopped pools cannot accept updates");
}
void errors_and_polling_checkpoint() {
    auto transport = std::make_shared<Dummy>(); std::atomic<int> completed{0};
    ConcurrentApplication pool(factory(transport, [&](Context& context) {
        if (context.update().update_id == 11) throw std::runtime_error("expected failure");
        ++completed;
    }), {2, 8, {}});
    transport->response = [] {
        return Json{{"ok", true}, {"result", Json::array({to_json(update(10, 1)), to_json(update(11, 2)), to_json(update(12, 3))})}};
    };
    Bot bot(transport); Polling polling(pool, bot);
    bool failed = false;
    try { polling.poll_once(); } catch (const std::runtime_error&) { failed = true; }
    require(failed && polling.offset() == 11, "concurrent polling must not acknowledge past the failed prefix");
    require(completed == 2, "accepted futures must all finish before returning a polling error");
    require(pool.submit(update(13, 2)).get() == Result::handled, "handler exceptions must not kill a worker");
    pool.shutdown();
}
void startup_cleanup_and_keys() {
    auto transport = std::make_shared<Dummy>(); int created = 0;
    bool rejected = false;
    try { ConcurrentApplication invalid([&]() -> std::unique_ptr<Application> {
        if (++created == 2) throw std::runtime_error("factory failure"); return std::make_unique<Application>(transport);
    }, {2, 1, {}}); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "factory failure must propagate without leaked worker threads");
    auto message = update(1, -1001234567890LL);
    require(chat_ordering_key(message) == static_cast<std::uint64_t>(-1001234567890LL), "64-bit chat key");
    CallbackQuery query{}; MaybeInaccessibleMessage parent{}; parent.value = Box<Message>(*message.message); query.message = parent;
    Update callback{}; callback.callback_query = query;
    require(chat_ordering_key(callback) == chat_ordering_key(message), "callbacks and messages in one chat must share a lane");
    require(chat_ordering_key(Update{}) == 0, "chatless events must share a stable fallback lane");
}
}
int main() {
    try { parallel_chats_and_ordering(); backpressure_and_draining(); errors_and_polling_checkpoint(); startup_cleanup_and_keys();
        std::cout << "Concurrent execution, FIFO ordering, backpressure, draining, exceptions, and checkpoints passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
