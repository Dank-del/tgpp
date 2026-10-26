#include <tgpp/ext/ext.hpp>
#include <atomic>
#include <iostream>

using namespace tgpp;
using namespace tgpp::ext;
struct NoNetwork : Transport { Json perform(const Request&) override { throw std::logic_error("benchmark has no network"); } };
int main() {
    constexpr int count = 1000;
    auto transport = std::make_shared<NoNetwork>();
    for (const auto workers : {1, 4}) {
        std::atomic<int> processed{0};
        ConcurrentApplication pool([&] {
            auto app = std::make_unique<Application>(transport);
            app->router().add<FunctionHandler>("simulated-io", filters::all(), [&](Context&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); ++processed;
            });
            return app;
        }, {std::size_t(workers), 256, {}});
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::future<Result>> results; results.reserve(count);
        for (int i = 0; i < count; ++i) {
            Update update{}; update.update_id = i; Message message{}; message.chat.id = i % 32 + 1;
            update.message = std::move(message); results.push_back(pool.submit(std::move(update)));
        }
        for (auto& result : results) result.get();
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        pool.shutdown();
        std::cout << Json{{"workers", workers}, {"updates", processed.load()}, {"seconds", seconds},
                          {"updates_per_second", count / seconds}, {"workload", "1ms simulated I/O per handler; no Telegram network"}}.dump() << '\n';
    }
}
