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

## HTTP client comparison

Build `tgpp_http_bench` with async support and benchmarks enabled, then run:

```bash
python3 bench/compare.py --driver build-async/tgpp_http_bench
```

This runs synchronous and coroutine clients against the same HTTP/1.1 local
Telegram-shaped server at concurrency 1, 8, 32, and 128. Each request waits 50 ms
on the server; each client maintains a closed loop with the specified concurrency.
The default is 512 requests and three independent process runs per configuration.
JSON results record throughput, p50/p95/p99 request latency, error count, process
CPU time and peak RSS on Unix, revision, working-tree dirtiness and platform.
The Python server is part of the workload and may become the bottleneck. These
results measure simulated latency, not Telegram rate limits or production traffic.

Competitor binaries can use the same simulator through `--adapters adapters.json`.
The file maps `tgbot-cpp`, `TGBM`, and `egorpugin/tgbot` to `revision` (an exact
40-character Git SHA) and `command` (an argv array). Command arguments may contain
`{base}`, `{requests}`, and `{concurrency}`. Every adapter must emit one JSON
object with the same measurement fields as the tgpp driver and use equivalent
request payloads, concurrency, deadlines and HTTP connection settings. Record
compiler, dependency revisions, build time and installed size alongside the run.
Adapters are launched directly, without a shell. Missing or failed adapters are
recorded explicitly; they are not evidence that tgpp outperforms that library.

Compare distributions across repeats, including low-concurrency regressions.
A 2x improvement over the four-worker baseline at concurrency 128 is a target for
controlled-machine evaluation, not a timing assertion in shared-runner CI.
The sync and async configurations at equal concurrency additionally distinguish
thread-count savings from increased permitted network parallelism.

Pinned competitor preparations and an outbound-only tgbot-cpp adapter are in
`bench/competitors`. The adapter compiles upstream API/curl sources unchanged,
omitting unrelated webhook/polling components. Prepare its dependencies and run:

```bash
python3 bench/competitors/prepare.py
python3 bench/compare.py --adapters build-competitors/adapters.json
```

`prepare.py` records exact upstream revisions, elapsed build attempts, and logs.
It attempts TGBM with already-installed dependencies (`TGBM_USE_CPM=OFF`) to avoid
an automatic Boost download, and uses the upstream SW command for egorpugin/tgbot.
Unavailable dependency/toolchain failures are reported instead of ranked. This
makes the current measured cross-library comparison tgbot-cpp versus tgpp; the
other two require their respective build environments before measurement.

For an actual old-version comparison, archive commit `8759ad3` to an ignored build
directory, use `bench/competitors/baseline.cmake` as a standalone CMakeLists, and
set `TGPP_BASELINE_SOURCE` to that checkout and `TGPP_BENCH_SOURCE` to this
repository's `bench/competitors` directory. The resulting `tgpp_baseline_bench`
accepts `BASE_URL REQUESTS CONCURRENCY`. The four-worker reference uses concurrency
4; equal-concurrency results should be reported separately.
