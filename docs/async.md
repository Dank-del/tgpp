# C++20 asynchronous bots

The version remains `0.0.1-rc1`. All targets now require C++20. Existing synchronous
clients and handlers remain available; asynchronous APIs are separate so a blocking
handler cannot accidentally stall an asynchronous application.

## Build and dependencies

```sh
cmake -S . -B build -DTGPP_BUILD_EXAMPLES=ON -DTGPP_WITH_WEBHOOK=ON -DTGPP_WITH_SQLITE=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix ./install
```

Standalone Asio 1.38.2 or newer (no Boost), libcurl, and nlohmann/json provide the
runtime, HTTP, and JSON implementations. The async curl transport requires an asynchronous DNS
resolver in libcurl. HTTP/2 is negotiated if the installed curl supports it; HTTP/1.1
also works. Optional webhook support adds the small llhttp C parser. Optional
SQLite support adds SQLite. Downloaded source archives are pinned by checksum.
When a suitable installed standalone Asio is unavailable, CMake fetches pinned
1.38.2. This release includes upstream ThreadSanitizer annotations for executor
reference counts, avoiding reports caused by TSan's unsupported standalone acquire
fences ([upstream implementation](https://github.com/chriskohlhoff/asio/blob/asio-1-38-2/include/asio/detail/atomic_count.hpp)).
Sanitizer checks remain enabled for SDK code; no race suppression is applied.
Disabling `TGPP_BUILD_ASYNC` builds only the synchronous SDK; curl can independently
be disabled for applications providing a custom transport.

Installed consumers use:

```cmake
find_package(tgpp CONFIG REQUIRED COMPONENTS async_ext)
target_link_libraries(my_bot PRIVATE tgpp::async_ext)
```

Other component targets are `tgpp::tgpp`, `tgpp::ext`, `tgpp::async`,
`tgpp::webhook`, and `tgpp::sqlite`. The install includes standalone Asio headers.

## Coroutine client and ownership

Include `<tgpp/async_methods.hpp>` and `<tgpp/async_curl.hpp>`. Construct a shared
`AsyncCurlTransport`, pass it to `AsyncBot`, and `co_await` the generated methods.
Request structs and attachments are owned by the coroutine; use normal request
construction and `std::move` to avoid unnecessary copies. Responses use the same
checked serializers and typed errors as the synchronous client.

```cpp
#include <tgpp/async_methods.hpp>
#include <tgpp/async_curl.hpp>

tgpp::Task<void> greet(tgpp::AsyncBot& bot, std::int64_t chat_id) {
    tgpp::SendMessageRequest message{};
    message.chat_id = chat_id;
    message.text = "Hello from a coroutine";
    auto control = std::make_shared<tgpp::RequestControl>();
    control->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto sent = co_await bot.send_message(std::move(message), {}, control);
    (void)sent;
}
```

The optional third argument is shared `RequestControl`, containing a `stop_token`
and an absolute steady-clock deadline. Low-level requests expose the same control:

```cpp
tgpp::Request request;
request.method = "getMe";
request.control = std::make_shared<tgpp::RequestControl>();
request.control->stop = stop_source.get_token();
auto result = co_await bot.call(std::move(request));
```

`stop_source` is an application-owned `std::stop_source`. Asio cancellation slots
also cancel queued or active curl operations. Each operation completes once on its
initiating executor. Keep the client/transport alive until its awaited calls finish.

`Runtime` defaults to two executor threads and two blocking workers. Submit explicit
blocking work with `co_await runtime.blocking_pool().run(callable)`. Admission is
bounded to 1,024 jobs by default; a full pool throws `OverloadError`. Coroutine
handlers must not block executor threads. A curl multi reactor drives many HTTP
transfers on a single additional thread. Each transport also owns two file-I/O
workers with capacity for 64 pending/active file jobs. Admission and active-transfer counts are
bounded and available through transport statistics. Excess admission fails with
`OverloadError`; callers decide whether to wait or reject their own work.

`RequestControl` carries a stop token and optional monotonic deadline for raw
transport requests. Cancellation interrupts queued/in-flight transfers and releases
curl resources. Destruction and shutdown must happen from an external thread;
application tasks and dependencies must finish before their runtime is destroyed.

`AsyncUploadSource` and `AsyncDownloadSink` allow incremental transfer with bounded
chunks and pause/resume backpressure. `FileUploadSource` and `FileDownloadSink` run
file operations through a supplied blocking pool. Streams are not automatically
replayed; supply a fresh source for another attempt. Download paths come from
Telegram's `getFile` response, not arbitrary external URLs. Treat cancellation
partway through an output file as a partial download.

```cpp
// runtime, bot, transport and a getFile result are supplied by the application.
tgpp::SendDocumentRequest document{};
document.chat_id = chat_id;
tgpp::InputFile upload;
upload.filename = "report.pdf";
upload.content_type = "application/pdf";
upload.source = std::make_shared<tgpp::FileUploadSource>(
    runtime.blocking_pool(), "report.pdf");
document.document = std::move(upload);
co_await bot.send_document(std::move(document));
auto sink = std::make_shared<tgpp::FileDownloadSink>(
    runtime.blocking_pool(), "download.pdf");
co_await transport->download(*file.file_path, sink);
```

For memory uploads, use `InputFile::from_memory(bytes, filename, content_type)`.
Application sources implement `read(max_bytes)` and return an empty chunk at EOF;
sinks implement `write(bytes)`. Chunk size is capped at 64 KiB. An external blocking
pool must outlive its adapters and operations; shared-pool adapter constructors
can own the pool instead. Application source/sink coroutines must cooperate with
shutdown: cancellation cannot interrupt arbitrary blocking developer code.

`AsyncCurlOptions` defaults to 32 transfer slots, 1,024 waiting requests, a 64 MiB
JSON response limit, 10-second connect timeout, 90-second transfer timeout, and
30-second queue timeout. One slot is reserved for polling when capacity exceeds
one. `stats()` reports active/queued/completed/failed counts; the optional observer
receives only method name, elapsed duration, and failure status on the caller's
executor. Observers must not block; thrown observer exceptions are ignored.

## Handlers and recovery

Include `<tgpp/ext/async.hpp>`. `AsyncApplication` owns its bot and dispatch state.
Register command callbacks returning `Task<void>` and `co_await context.reply(...)`.
Middleware returns `Task<Result>` and awaits `AsyncNext` at most once. Module
start/stop hooks are asynchronous. Sessions middleware exposes mutable JSON through
`context.get<SessionData>().value` and saves after successful dispatch.

Dispatch serializes the entire handler lifetime for a key, including time spent
awaiting network calls. Different keys can progress concurrently. Defaults are 64
active keys and 1,024 queued updates; custom key functions support user/business
scopes. Handler state shared across keys must be safe for concurrent access.

Polling retries temporary retrieval failures with capped exponential backoff and
jitter, honoring Telegram `retry_after`. Handler failures and permanent API errors
propagate through the configured error policy. Outgoing sends are never silently
retried: a lost response could conceal a successful send. Logs and metrics should
record exception categories and counts without bot tokens or message contents.

Stop polling, stop accepting updates, and await application shutdown before
releasing services. SDK operations cooperate with cancellation; arbitrary developer
code must also cooperate and avoid blocking indefinitely.

## Webhook deployment

`WebhookServer` accepts an asynchronous `Receiver(const Update&)` callback. Configure an
explicit path and a nonempty secret token; the default listener is loopback HTTP
on port 8080. A reverse proxy such as Caddy terminates HTTPS:

```caddyfile
bot.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

Register the public HTTPS URL and the matching `secret_token` explicitly with
`setWebhook`; the SDK does not alter remote webhook registration automatically.
Run webhook ingestion or polling for a given bot, not both. The reverse proxy
must retain `X-Telegram-Bot-Api-Secret-Token`.

The callback returns only after durable enqueue or successful processing. Failed
admission/processing leaves delivery unacknowledged for Telegram retry. Authentication,
parsing, request size, receive time, and simultaneous-connection limits are enforced.
Connections close after each response. No direct certificate/TLS configuration is
needed in the SDK listener.

## Durable updates and sessions

`MemoryStores` provides transient sessions and an inbox. Optional `SqliteStores`
provides durable sessions, inbox, and polling checkpoints on a dedicated database
worker, using WAL and schema versioning. SQLite targets a single bot process.

With a store configured, polling persists each retrieved batch and its next offset
atomically. Processing occurs separately through `process_pending()`; run processor
loops as well as the poller. Webhook receivers can enqueue decoded updates in the
same store before returning success. Always use the same bot identity and ordering
key across ingestion and processing.

Updates are deduplicated by bot identity and update ID. Workers claim only the
first unfinished update for each key, with leases and fencing tokens. Renew leases
while processing. Five failed/expired attempts block that key; an operator can
requeue or skip the retained failed update. Other keys continue. Completed update
records retain deduplication history; plan database retention explicitly for
long-running bots.

Delivery is at least once. Sending a Telegram message, saving sessions, and completing
an inbox update cannot form one atomic transaction. Handlers must tolerate replay.
Versioned session compare-and-swap detects conflicting writes rather than overwriting
them silently. Expired sessions read as absent; revisions still prevent stale writers.

For multiple processes, implement `SessionStore` and `UpdateStore` against your
chosen shared system. Preserve atomic batch/checkpoint updates, unique enqueue,
per-key ordering, lease ownership, and fencing. Run one coordinated poller per bot
and multiple processing workers; distribute webhook reception through the shared
inbox. Never use process-local session locks as distributed coordination. Reuse
`tests/store_contract.hpp` when validating an adapter. No database server dependency
is imposed on developers who only need one process.
