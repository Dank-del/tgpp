# Comfortable syntax with full API control

Use `tgpp::ext::BotApp` for the usual bot startup and handwritten fluent helpers
for common sends. The complete generated request types and all Bot API methods
remain available through `AsyncBot` and `BotApp::application()`.

```cpp
#include <tgpp/ext/bot_app.hpp>
#include <cstdlib>
using namespace tgpp::ext;

int main() {
    const auto* token = std::getenv("TELEGRAM_BOT_TOKEN");
    if (!token) return 1;
    BotApp app(token);
    app.command("start", reply("Welcome!"));
    app.run();
}
```

`BotApp` owns the runtime, transport, application, and poller. `run()` manages
startup, signal handling, polling recovery, and shutdown. Construction and
registration make no API calls. By default `run()` first calls `getMe`; set
`BotAppOptions::bootstrap_identity=false` to skip that step. An explicitly
configured bot username is preserved. Startup does not register commands or
change webhooks. Supply a custom transport directly with
`BotApp(std::shared_ptr<AsyncTransport>, options)`; this constructor also works
with curl disabled. Use the separate `Runtime`/`AsyncApplication` components when
embedding the SDK in an existing event loop. The version remains `0.0.1-rc1`; C++20 is
required.

## Messages and chat-aware replies

```cpp
using namespace tgpp;

co_await bot.to(chat_id).text("<b>Hello</b>").html().silent().send();
// BotApp forwards .to(chat_id) too, so app.to(chat_id).text(...).send() works.
co_await bot.to("@channel").photo("existing-file-id").caption("Hello").send();
co_await bot.to(chat_id).document(InputFile{"report.pdf"}).caption("Report").send();

co_await ctx.reply("Hello");
co_await ctx.say(html::bold("Hello <user>"))
    .html()
    .keyboard(keyboard({{callback_button("Next", "next"),
                         url_button("Docs", "https://example.com")}}))
    .send();
```

`ctx.say()` initializes chat, thread/topic, business connection, and reply-to
metadata from the current update, just like `ctx.reply()`. Guest and ephemeral
replies require explicit generated API methods. The builder owns its values and
a copy of the client. Its `send()` creates an independently owned task, so it is
safe to await that task after a temporary builder or the original client has
been destroyed. The task does not retain the context.

Builders support `.html()`, `.markdown_v2()`, `.silent(bool)`,
`.protect_content(bool)`, `.keyboard(markup)`, `.reply_to(message_id)`,
`.thread(thread_id)`, `.business(connection_id)`, `.control(request_control)`,
and `.attachments(named_files)`. Photo/document builders also support `.caption()`.
Flags default to `true` when called without an argument; explicit `false` is
preserved in the outgoing payload.

Use `html::escape`, `bold`, `italic`, `code`, and `link(label, url)` for dynamic
content. Each helper escapes user text; `link` also escapes its URL attribute.
These helpers construct HTML rather than enabling a parse mode automatically.
Use `.html()` on the builder. Pass plain user text to the helpers to avoid double
escaping.

## Every generated option remains accessible

`.configure(fn)` synchronously mutates the full typed request, keeping all
previous defaults and builder values. Later calls take precedence:

```cpp
co_await ctx.say("A message")
    .silent()
    .configure([](SendMessageRequest& request) {
        request.disable_notification = false;
        request.message_thread_id = 42;
        request.business_connection_id = "another-connection";
        request.allow_paid_broadcast = false;
        request.link_preview_options = LinkPreviewOptions{};
        request.link_preview_options->is_disabled = true;
    })
    .send();
```

`.request()` returns the full generated request by reference while the builder
lives. `.request(full_request)` replaces it completely, including chat ID and
any reply defaults. Both are escape hatches for all current and future schema
fields. Named attachments and cancellation/deadline controls remain separate
and are forwarded unchanged:

```cpp
auto control = std::make_shared<RequestControl>();
control->stop = stop_source.get_token();
control->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

auto message = bot.to(chat_id).text("Configured");
message.request().message_effect_id = "effect-id";
auto result = co_await message.control(control).send();

SendPhotoRequest request{};
request.chat_id = chat_id;
request.photo = "attach://image";
co_await bot.send_photo(request,
    {{"image", InputFile{"image.png"}}}, control);
```

The helpers perform no network I/O during configuration. `send()` returns a lazy
`Task<Message>`; awaiting or spawning it performs exactly one API call. The layer
adds no retries. Transport injection, explicit request construction, streaming
sources, request deadlines/cancellation, and the generated low-level methods
remain usable together with the convenience layer.

## Handler shortcuts and composable filters

```cpp
using namespace tgpp::ext;

app.command("start", reply("Welcome!"));
app.add("private-text", async_filters::private_chat() & async_filters::text(), echo());
app.add("buttons", async_filters::callback_prefix("show:"), answer("Done"));
```

Here `app` is an `AsyncApplication` (use `bot_app.application()` to access it
from `BotApp`). `BotApp` also offers `.on_text(echo())` and
`.on_callback("show:", answer("Done"))`.

`reply`, `echo`, and `answer` return ordinary `AsyncCallback` values. The filter
helpers return predicates that compose with `&`, `|`, and `!`, using short-circuit
boolean evaluation. `all`, `message`, `text`, `private_chat`, `command`, and
`callback_prefix` are provided. Custom filters and handlers remain first-class
registration arguments; helpers do not replace them.

## Automatic builders for every Telegram method

`bot.api()` (also forwarded directly by `BotApp::api()`) exposes a typed builder for **every method and field in the pinned
schema**, rather than a manually chosen subset. Schema refreshes generate new
method factories and field setters
alongside the raw client:

```cpp
auto sent = co_await bot.api().send_message()
    .chat_id(chat_id)
    .text("All options available")
    .disable_notification(false)
    .protect_content(true)
    .parse_mode("HTML")
    .message_thread_id(42)
    .send();

auto me = co_await bot.api().get_me().send();
co_await bot.api().delete_webhook().drop_pending_updates(false).send();

// Optional fields can be reset without discarding the rest of the request.
auto message = bot.api().send_message().chat_id(chat_id).text("Plain text");
message.parse_mode("HTML").parse_mode(std::nullopt);
co_await message.send();
```

Each factory and field uses the corresponding snake_case API name. Setters are
typed from the same schema as raw requests. Optional setters distinguish ordinary
values from explicit reset: `.allowed_updates({})` sends an empty array,
`.caption({})` sends an empty string, and `false`/zero remain explicit values.
Use `.field(std::nullopt)` to clear a field. Explicit `std::optional<T>` values
are accepted too, preserving engaged or empty state. Arrays, union alternatives,
media uploads, and all return types keep
the generated C++ types. `send()` returns the method's actual `Task<Result>`.

Named required setters track whether required arguments were supplied. Calling
`send()` before all required arguments are set throws before any I/O. Presence
tracking does not validate Telegram's field contents or cross-field constraints.
Explicit `.configure(fn)`, `.request(full_request)`, or mutable `.request()` access
opts into the raw API's completeness responsibility, allowing complete control of
all fields without trying to infer mutations inside user code. Controls and named
attachments are forwarded through `.control()` and `.attachments()` just as with
the curated builders. Every terminal task owns its client and request data.

To protect terminal and escape-hatch APIs, a schema field named `send`, `request`,
`configure`, `control`, or `attachments` receives a `set_` prefix (for example,
`.set_send(...)`). Internal helper-name collisions receive the same treatment.
C++ keywords receive a trailing underscore, such as `.requires_(...)`. Ambiguous
aliases or a new method that conflicts with a client entry point stop generation
with an actionable compatibility error instead of producing broken C++.

## Stability across schema refreshes

The curated convenience implementation is handwritten. Generation only declares
the stable `AsyncBot::to` bridge and includes the handwritten header. The separate
`AsyncBot::api` bridge includes a schema-generated builder surface. Adding optional
Telegram fields automatically exposes them as named setters in `bot.api()` and
through generated requests and `.configure()`/`.request()` escape hatches.

CI compiles a deliberately stable user-style fixture and verifies fluent payload
parity, owned task lifetimes, explicit option overrides, required argument presence,
and the generator's handling of new optional fields and entirely new methods.
A separate test generates isolated headers for a synthetic future API and compiles
the frozen fluent examples and newly generated calls against them. The scheduled
schema-update workflow runs those checks before proposing an update. If Telegram
removes or changes a field used by a curated shortcut, the update must preserve
that shortcut where its meaning remains supported, or require an explicit
compatibility decision; these helpers cannot keep a removed Telegram feature
working.
