#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <tgpp/async_curl.hpp>
#include <tgpp/async_methods.hpp>
#include <tgpp/methods.hpp>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#endif
using Clock = std::chrono::steady_clock;

std::size_t thread_count() {
#ifdef __linux__
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++count;
    }
    return count;
#else
    return 0;
#endif
}

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            throw std::invalid_argument(
                "usage: tgpp_http_bench sync|baseline-default|async BASE_URL REQUESTS CONCURRENCY");
        }
        std::string mode = argv[1], base = argv[2];
        auto count = std::stoul(argv[3]), concurrency = std::stoul(argv[4]);
        if (!count || !concurrency ||
            (mode != "sync" && mode != "baseline-default" && mode != "async")) {
            throw std::invalid_argument("invalid benchmark arguments");
        }
        std::vector<double> latency(count);
        std::atomic<std::size_t> next = 0, failed = 0, peak_threads = 0;
        Clock::time_point start;
        auto observe_threads = [&] {
            auto n = thread_count();
            auto prev = peak_threads.load();
            while (prev < n && !peak_threads.compare_exchange_weak(prev, n)) {
            };
        };
        auto record = [&](std::size_t i, Clock::time_point begin) {
            latency[i] = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        };
        if (mode == "sync" || mode == "baseline-default") {
            auto workers_count =
                mode == "baseline-default" ? std::min(concurrency, std::size_t(4)) : concurrency;
            tgpp::CurlOptions options;
            options.base_url = base;
            options.max_connections = concurrency;
            tgpp::Bot bot(std::make_shared<tgpp::CurlTransport>("benchmark", options));
            start = Clock::now();
            std::vector<std::thread> workers;
            for (std::size_t w = 0; w < workers_count; ++w) {
                workers.emplace_back([&] {
                    observe_threads();
                    while (true) {
                        auto i = next++;
                        if (i >= count) {
                            return;
                        }
                        auto begin = Clock::now();
                        try {
                            bot.get_me();
                        } catch (...) {
                            ++failed;
                        }
                        record(i, begin);
                    }
                });
            }
            for (auto& thread : workers) {
                thread.join();
            }
        } else {
            tgpp::Runtime runtime;
            tgpp::AsyncCurlOptions options;
            options.base_url = base;
            // The transport reserves one slot for polling. Keep the same number
            // of general request slots as the other clients in this benchmark.
            options.max_connections = concurrency + (concurrency > 1 ? 1 : 0);
            options.max_pending = count + 1;
            tgpp::AsyncBot bot(std::make_shared<tgpp::AsyncCurlTransport>("benchmark", options));
            auto worker = [&]() -> tgpp::Task<void> {
                observe_threads();
                while (true) {
                    auto i = next++;
                    if (i >= count) {
                        co_return;
                    }
                    auto begin = Clock::now();
                    try {
                        co_await bot.get_me();
                    } catch (...) {
                        ++failed;
                    }
                    record(i, begin);
                }
            };
            start = Clock::now();
            std::vector<std::future<void>> workers;
            for (std::size_t w = 0; w < concurrency; ++w) {
                workers.push_back(asio::co_spawn(runtime.executor(), worker(), asio::use_future));
            }
            for (auto& future : workers) {
                future.get();
            }
        }
        auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::sort(latency.begin(), latency.end());
        auto percentile = [&](double q) {
            return latency[std::min(latency.size() - 1,
                                    std::size_t(q * double(latency.size() - 1)))];
        };
        tgpp::Json result{{"library", "tgpp"},
                          {"mode", mode},
                          {"requests", count},
                          {"concurrency", concurrency},
                          {"seconds", seconds},
                          {"throughput", count / seconds},
                          {"p50_ms", percentile(.5)},
                          {"p95_ms", percentile(.95)},
                          {"p99_ms", percentile(.99)},
                          {"errors", failed.load()},
                          {"peak_threads", peak_threads.load()},
                          {"baseline_reference", "8759ad3"}};
#ifndef _WIN32
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        result["cpu_seconds"] = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                                usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#ifdef __APPLE__
        result["peak_rss_bytes"] = usage.ru_maxrss;
#else
        result["peak_rss_bytes"] = usage.ru_maxrss * 1024;
#endif
#endif
        std::cout << result.dump() << '\n';
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
