# Dispatch benchmark

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTGPP_BUILD_BENCHMARKS=ON
cmake --build build --target tgpp_dispatch_bench --parallel 2
./build/tgpp_dispatch_bench
```

The benchmark submits 1,000 updates across 32 chats. Each handler waits for
one millisecond to simulate an external I/O operation. It compares one worker
with four workers, includes submission/backpressure/future completion, and
does not call Telegram or open HTTP connections. The queue capacity is 256
updates per worker. It makes no assertion about real Telegram throughput.

Observed locally on Linux, GCC 16.2.1, default CMake build with no optimization
flags, while developing `0.0.1-rc1`:

| Workers | Elapsed | Updates/second |
| --- | ---: | ---: |
| 1 | 1.114 s | 897 |
| 4 | 0.310 s | 3,229 |

That is approximately 3.6× throughput on this simulated I/O workload. CPU-bound
work, queue-key distribution, thread scheduling, actual network latency, and
Telegram rate limits produce different results. Run an optimized build and a
workload representative of your application when choosing worker and pool sizes.

The HTTP integration test separately verifies both TCP connection reuse and
overlapping requests bounded by a two-handle pool. The concurrency test checks
parallel execution with synchronization barriers rather than timing-based
speed assertions.
