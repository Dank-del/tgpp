# Fetch tgpp from GitHub

This is a standalone CMake consumer. CMake fetches the public GitHub repository
at `v0.0.1-rc1`, supplies the JSON dependency, and links the bot framework with
`tgpp::ext`. The executable uses an offline transport, so no bot token is needed.
Install the libcurl development package first, as described in the root README.

From the repository root:

```bash
cmake -S examples/fetchcontent -B build/fetchcontent -G Ninja
cmake --build build/fetchcontent --parallel 2
./build/fetchcontent/my_bot
```

Copy `CMakeLists.txt` and `main.cpp` to a new directory to use them independently
of a local tgpp checkout. Use `tgpp::tgpp` for the direct API client, or `tgpp::ext`
for modules, handlers, polling, and concurrency; the latter includes the client
dependency transitively. The target also propagates its C++20 requirement and
header/dependency include paths.

Dependency tests default off through `TGPP_BUILD_TESTS`, independently of the
caller's `BUILD_TESTING` value and test targets. Use `TGPP_BUILD_TESTS=ON` only
when explicitly opting in to tgpp's own test targets.
