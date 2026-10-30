# Async showcase

The same33 message demos as `showcase_bot`, using `AsyncBot`, coroutine handlers,
asynchronous timers, and background per-chat jobs. Callback acknowledgements and
`/stop` remain independent of an active tour. This is an example, not a benchmark.

```sh
cmake -S . -B build -DTGPP_BUILD_ASYNC=ON -DTGPP_BUILD_EXAMPLES=ON
cmake --build build --target async_showcase_bot --parallel 2
TELEGRAM_BOT_TOKEN=... ./build/async_showcase_bot
```

Open a private chat and use `/showcase`, `/demo photo`, `/tour`, `/report`, or
`/stop`. Eight chat sessions can be active; each admits at most three queued jobs.
An idle session is evicted when another chat needs its slot. `/stop` clears queued
work and cancels the active coroutine, including cancellable network operations.
Ctrl+C/SIGTERM stops polling, drains handlers, and cancels/joins background jobs.
Run one poller per bot token. The example checks an existing webhook and leaves it
unchanged; remove it explicitly before switching to polling.

Assets are reused from `examples/showcase_bot/assets`. Override with
`TGPP_SHOWCASE_ASSETS`. Per-chat JSON reports are stored under
`build/async_showcase_reports`, configurable with `TGPP_ASYNC_SHOWCASE_REPORT_DIR`.
Report reads/writes use the runtime's bounded blocking pool. Writes replace the
previous file through a temporary file and atomic rename (Windows uses replace
semantics). Invalid existing reports are quarantined with a `.corrupt` suffix
when possible, and the menu/tour starts with an empty report. Reports contain at
most33 results and error strings are truncated; report writes are best-effort
example output, not a durable update inbox. See `docs/persistence.md` for durable
processing. No live API call is made by the offline catalogue test.

The `generated/` sources derive from the maintained synchronous catalogue:

```sh
python examples/async_showcase/generate.py
python examples/async_showcase/generate.py --check
```

The transform inserts coroutine return types and awaits into the existing demos;
the shared API schema still generates both SDK clients. Keep the pinned development
formatters available when updating/checking the showcase. All33 async demos are
executed against a fake async transport in `async_showcase_tests`, checking file
uploads, Unicode entities, rich-message blocks, media groups, file-ID reuse, and
callback-data sizes. Actual Telegram restrictions are recorded per demo at runtime.
