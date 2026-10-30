# Changelog

## 0.0.1-rc1

First release candidate. The version stays unchanged during the async upgrade.

- C++20 coroutine client generated from the same pinned schema, using standalone
  Asio and libcurl multi without Boost.
- Ordered async handlers, scoped routers, middleware and lifecycle hooks;
  bounded scheduling, cancellation, deadlines and polling recovery.
- Streamed uploads/downloads and bounded blocking file helpers.
- Optional reverse-proxy webhook listener and SQLite sessions/durable inbox;
  versioned sessions, leases and fencing for developer-supplied distributed stores.
- Installed CMake packages, Linux/macOS/Windows workflow coverage and local HTTP
  benchmark drivers with pinned competitor build recipes.

- Generated API coverage for Bot API 10.3: 400 types, including the
  upload runtime type, and 185 methods from a pinned, checksum-verified spec.
- Class-based commands, callback handlers, nested routing, middleware,
  feature modules, lifecycle hooks, and explicit error policies.
- Bounded concurrent application workers with chat-ordering keys, producer
  backpressure, futures, draining shutdown, and ordered polling checkpoints.
- A bounded curl handle pool with persistent connection reuse and parallel
  request support.
- Modular, concurrent, and public showcase bot examples; 33 live message demos.
- Architecture diagrams, reproducible dispatch benchmarks, and GCC/Clang CI.
- Direct GitHub `FetchContent` integration with public CMake targets, isolated
  test options, and a standalone consumer example.
- Consistent C++, Python, and CMake formatting, pinned development tools,
  formatter-aware code generation, and CI formatting checks.

The canonical version is stored in `VERSION`. `TGPP_VERSION` retains the full
prerelease string; CMake's numeric `PROJECT_VERSION` is `0.0.1`.
