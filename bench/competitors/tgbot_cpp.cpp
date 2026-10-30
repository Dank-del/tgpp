#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <tgbot/Api.h>
#include <tgbot/CurlHttpClient.h>
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
        if (argc != 4) {
            throw std::invalid_argument("usage: tgpp_http_bench BASE_URL REQUESTS CONCURRENCY");
        }
        std::string base = argv[1];
        const std::size_t count = std::stoul(argv[2]);
        const std::size_t concurrency = std::stoul(argv[3]);
        if (!count || !concurrency) {
            throw std::invalid_argument("invalid benchmark arguments");
        }
        std::vector<double> latency(count);
        std::atomic<std::size_t> next = 0, failed = 0, peak_threads = 0;
        Clock::time_point start;
        auto record = [&](std::size_t i, Clock::time_point begin) {
            latency[i] = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        };
        {
            TgBot::CurlHttpClient http;
            TgBot::Api bot("benchmark", http, base);
            start = Clock::now();
            std::vector<std::thread> workers;
            for (std::size_t w = 0; w < concurrency; ++w) {
                workers.emplace_back([&] {
                    auto n = thread_count();
                    auto prev = peak_threads.load();
                    while (prev < n && !peak_threads.compare_exchange_weak(prev, n)) {
                    };
                    while (true) {
                        auto i = next++;
                        if (i >= count) {
                            return;
                        }
                        auto begin = Clock::now();
                        try {
                            bot.getMe();
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
        }
        auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::sort(latency.begin(), latency.end());
        auto percentile = [&](double q) {
            return latency[std::min(latency.size() - 1,
                                    std::size_t(q * double(latency.size() - 1)))];
        };
        nlohmann::json result{{"library", "tgbot-cpp"},
                              {"mode", "sync"},
                              {"requests", count},
                              {"concurrency", concurrency},
                              {"seconds", seconds},
                              {"throughput", count / seconds},
                              {"p50_ms", percentile(.5)},
                              {"p95_ms", percentile(.95)},
                              {"p99_ms", percentile(.99)},
                              {"errors", failed.load()},
                              {"peak_threads", peak_threads.load()}};
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
