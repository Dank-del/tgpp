# Modular bot example

This is a small bot organized like an application that can grow. `/start` lives
in the welcome feature. `/add <title>` and `/tasks` live in the tasks feature;
their handler classes share an injected domain service and repository.

Build from the tgpp repository root:

```bash
cmake -S . -B build -G Ninja -DTGPP_BUILD_EXAMPLES=ON -DCMAKE_CXX_STANDARD=17
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
# Set TELEGRAM_BOT_TOKEN in the environment, then:
./build/examples/modular_bot/modular_bot
```

Start with [app/main.cpp](app/main.cpp) to see dependency wiring, then
[features/tasks/tasks_module.cpp](features/tasks/tasks_module.cpp) to see
feature registration. The task service depends on a repository interface and
has no tgpp dependency. The CMake target `modular_bot_domain` therefore builds
without linking to Telegram or the framework.

The in-memory repository is example storage: tasks disappear on process exit.
Task commands accept private chats and isolate tasks by user. Each feature has
its own handlers and domain code; shared implementation details belong in
`infrastructure/`, and cross-cutting update behavior belongs in `middleware/`.

The example stops on unhandled API/transport/handler errors. Update the error
policy and persistence adapter for your application. Ctrl+C takes effect after
the current long poll or HTTP call finishes.

See the [architecture guide](../../docs/architecture.md) for routing order,
lifecycle, middleware, testing, and extension points.
