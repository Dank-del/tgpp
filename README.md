# tgpp

Current version: **0.0.1-rc1**.

A generated C++17 client for the Telegram Bot API, using the same
[JSON specification](https://github.com/PaulSonOfLars/telegram-bot-api-spec)
as [gotgbot](https://github.com/PaulSonOfLars/gotgbot).

The pinned **Bot API 10.3** schema produces **400 types** (including the
handwritten `InputFile` upload type) and **185 methods**. Generated code is
checked in. Consumers need no Python or generator at build time.

## Organizing a larger bot

The `tgpp::ext` framework adds class-based commands, feature modules, nested
routers, middleware, lifecycle hooks, centralized error handling, and long
polling. `ConcurrentApplication` provides bounded worker queues with ordering
within each chat; the HTTP transport pools reusable curl handles and connections.
Link `tgpp::ext` and include `<tgpp/ext/ext.hpp>`.

Use one feature folder per capability, with small handler classes calling
injected services and repository interfaces. The runnable
[modular bot example](examples/modular_bot) demonstrates this with welcome and
tasks features, configuration, logging middleware, and offline tests.

```text
app/                         # Startup and dependency wiring
features/tasks/handlers/     # One class per command
features/tasks/domain/       # Business rules and repository interface
features/welcome/            # Another independent feature
infrastructure/              # Database and external service adapters
middleware/                  # Shared update behavior
tests/                       # Domain and feature tests
```

See the [architecture guide](docs/architecture.md) for the public classes,
registration examples, ordering, ownership, and error policies.

To see the message types in Telegram, run the [showcase bot](examples/showcase_bot):
33 demos with an interactive menu, bundled media, independent chat sessions,
background tour workers, and a live API result report for each chat.

```bash
python3 tools/run_example.py showcase_bot --tour
```

## Build

Requires CMake 3.24+, a C++17 compiler, and libcurl development headers (7.56+).
The JSON dependency is nlohmann/json: CMake uses an installed version (3.11+)
or downloads the pinned, checksum-verified 3.12.0 release.

```bash
# Ubuntu / Debian
sudo apt-get install cmake ninja-build g++ libcurl4-openssl-dev
# Fedora
sudo dnf install cmake ninja-build gcc-c++ libcurl-devel

cmake -S . -B build -G Ninja -DTGPP_BUILD_EXAMPLES=ON -DCMAKE_CXX_STANDARD=17
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

The generated surface is large; two parallel compiler jobs keep memory use
reasonable. For a custom transport without libcurl, configure with
`-DTGPP_WITH_CURL=OFF` and leave examples disabled.

## Add to your CMake project

Use CMake's [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html)
to download tgpp directly from GitHub, pinned to a release:

```cmake
cmake_minimum_required(VERSION 3.24)
project(my_bot LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(tgpp
    GIT_REPOSITORY https://github.com/Dank-del/tgpp.git
    GIT_TAG v0.0.1-rc1
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(tgpp)

add_executable(my_bot main.cpp)
target_link_libraries(my_bot PRIVATE tgpp::ext)
```

`tgpp::ext` includes the generated client and the application framework.
For only the API client, link `tgpp::tgpp`. CMake supplies the JSON dependency;
libcurl development headers and libraries must be installed on your system.
See the [standalone consumer example](examples/fetchcontent) for a complete
project that builds and runs without a bot token.

For a local checkout, use:

```cmake
add_subdirectory(path/to/tgpp)
target_link_libraries(my_bot PRIVATE tgpp::tgpp)
```

Tests default off when tgpp is fetched as a dependency;
`TGPP_BUILD_TESTS` controls them independently of the parent project's tests.
Generator maintenance targets are defined only in standalone tgpp builds.

## Send a message

```cpp
#include <tgpp/tgpp.hpp>
#include <cstdlib>

int main() {
    const char* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token) return 1;

    tgpp::Bot bot(std::make_shared<tgpp::CurlTransport>(token));
    tgpp::SendMessageRequest message{};
    message.chat_id = std::int64_t(123456789);
    message.text = "Hello from C++17!";
    auto sent = bot.send_message(message);
    return sent.message_id == 0 ? 1 : 0;
}
```

Run the long polling echo bot with `TELEGRAM_BOT_TOKEN` set:

```bash
./build/echo_bot
```

Stop with Ctrl+C; an in-flight poll may take up to 30 seconds to finish.
The small echo example exits on API or transport errors. For feature modules
and class-based handlers, build and run `./build/examples/modular_bot/modular_bot`.
`get_updates` and webhook registration methods are generated. A webhook server
can feed updates into `Application::process_json()` using the same modules;
the HTTP listener is supplied by your chosen server stack.

For local credentials, copy `.env.example` to `.env` and set the token there.
The `.env` file is ignored by version control. Run examples with it using:

```bash
python3 tools/run_example.py modular_bot
```

To send a single test message, also set `TELEGRAM_TEST_CHAT_ID` to the numeric
ID of a private chat that has started your bot, then run:

```bash
python3 tools/run_example.py send_message "Hello from tgpp!"
```

The launcher treats `.env` values as data and gives them precedence over the
inherited environment. Telegram usernames are not private chat IDs.

## Types and serialization

| API concept | C++ representation |
| --- | --- |
| Integer, including identifiers | `std::int64_t` |
| Float / String / Boolean | `double` / `std::string` / `bool` |
| Optional field | `std::optional<T>` |
| Array | `std::vector<T>` |
| Multiple possible field or return types | `std::variant<...>` |
| Named union, such as `ReactionType` | Struct with a `std::variant` member named `value` |
| Recursive object dependency | `Box<T>` with deep-copy value semantics |

Optional fields are omitted when unset; explicitly setting `false`, zero, an
empty string, or an empty array preserves that value. Unknown object fields
are ignored when decoding. Required fields and JSON value kinds are checked.
Known discriminator strings get generated defaults and validation. Unknown
union alternatives are rejected; update the schema when Telegram adds one.

Most named unions use discriminator fields such as `type` or `status`.
`MaybeInaccessibleMessage` uses Telegram's `date == 0` rule. Unions without a
discriminator try the shape with the most required fields first, then schema
order. Structurally ambiguous inputs therefore require application care.

```cpp
auto update = tgpp::from_json<tgpp::Update>(json);
auto json_again = tgpp::to_json(update);
```

The generator handles field presence and representation. Telegram enforces
length limits, numeric ranges, and conditions involving multiple fields.

## Uploads

```cpp
tgpp::SendDocumentRequest document{};
document.chat_id = std::int64_t(123456789);
document.document = tgpp::InputFile{"./report.pdf", "report.pdf", "application/pdf"};
bot.send_document(document);
```

`InputFile` becomes a multipart attachment automatically. Existing file IDs
and URLs use the string alternative. For string `attach://` references inside
nested media, supply explicitly named attachments:

```cpp
tgpp::InputMediaPhoto photo{};
photo.media = "attach://photo";
tgpp::InputMediaVideo video{};
video.media = "attach://video";
tgpp::SendMediaGroupRequest album{};
album.chat_id = std::int64_t(123456789);
album.media = {photo, video};
bot.send_media_group(album, {
    {"photo", tgpp::InputFile{"./photo.jpg", "photo.jpg", "image/jpeg"}},
    {"video", tgpp::InputFile{"./video.mp4", "video.mp4", "video/mp4"}}
});
```

## Errors and custom transports

API failures throw `ApiError`, carrying `code`, optional `retry_after`, and
optional `migrate_to_chat_id`. Transport and malformed response errors throw
standard exceptions. Retries are explicit so a lost response does not silently
duplicate a send operation.

Implement `Transport::perform(const Request&)` to use another HTTP stack or
mock the network. It returns the full Telegram JSON envelope. `make_request`
overloads expose generated parameters and extracted uploads for inspection.

The curl transport keeps TLS verification enabled, disables redirects, uses
bounded timeouts, and leases reusable handles from a bounded pool. Set `CurlOptions`
to configure the API base URL and timeouts; the HTTP timeout must exceed the
long polling timeout. `max_connections` and `pool_timeout_seconds` control
HTTP concurrency. It supports file paths, not streaming upload callbacks.

For concurrent polling, see [examples/concurrent.cpp](examples/concurrent.cpp)
and the [concurrency diagrams and contracts](docs/architecture.md#concurrent-applications).
Each worker gets its own application and handlers. Shared services must support
concurrent access. Polling advances only a successful prefix of completed updates.
See the [benchmark workload and measured results](bench/README.md) for a
reproducible comparison of one and four application workers.

## Generation and updates

Python 3.9+ is needed only to maintain generated code. No Python packages are
required.

```bash
python3 tools/generate.py                # offline, using spec/api.json
python3 tools/generate.py --check        # verify checksum and reproducibility
python3 tools/generate.py --update-spec  # fetch upstream pin and regenerate
```

`spec/lock.json` records the upstream commit and SHA256. Review generated
changes and rebuild/test before publishing an update. The generator fails on
unknown schema types instead of substituting untyped JSON. Generation is
deterministic; there are no timestamps in generated output.

`tools/generate.py` contains the schema-to-C++ translation, dependency ordering,
recursive component detection, and union handling. `include/tgpp/core.hpp`,
`codec.hpp`, and `src/curl_transport.cpp` contain the handwritten runtime.
This structure leaves room for a separate C emitter using the same spec.

Tests compile the complete generated surface and exercise optional fields,
64-bit IDs, recursive copies, tagged and structural unions, API error metadata,
JSON requests, and binary multipart uploads against a local HTTP server.
They do not require a live bot token. CI builds with GCC and Clang.
Framework tests also exercise middleware ordering, lifecycle cleanup, error
policies, polling checkpoints, and the modular example's dependency boundaries.

## License

MIT. The vendored API specification retains its upstream license in
[`spec/LICENSE`](spec/LICENSE).
