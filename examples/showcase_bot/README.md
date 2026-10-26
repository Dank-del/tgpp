# Telegram message showcase

A running example for `tgpp::ext`: class-based handlers and a scoped module
dispatch work to independent private-chat sessions, so a long media tour does
not block callback acknowledgements, `/stop`, or other people's demos.

## Run

Build from the repository root with `TGPP_BUILD_EXAMPLES=ON`. Set
`TELEGRAM_BOT_TOKEN` in the ignored `.env` file. Anyone can start a private chat
with the bot and use `/showcase` or `/tour`; an allowlist is not required.
Ordinary text also opens the demo menu.
`TELEGRAM_TEST_CHAT_ID` is optional and only selects a chat to receive a startup
menu or the automatic `--tour`. It does not restrict who can use the bot.

```bash
cmake --build build --parallel 2
python3 tools/run_example.py showcase_bot
```

Use `--tour` with `TELEGRAM_TEST_CHAT_ID` set to start a tour automatically.
Without a startup chat, the bot waits for incoming private messages.
The bot keeps polling until
Ctrl+C or SIGTERM. Do not run another long poller with the same bot token.
An existing webhook is detected and left in place.

For the local workspace running this example as `tgpp-showcase.service`, use
`systemctl --user status tgpp-showcase` to inspect it and
`systemctl --user stop tgpp-showcase` to stop the process. `/stop` only cancels
demo work; it leaves the bot online.

| Command | Behavior |
| --- | --- |
| `/start`, `/help`, `/showcase` | Show the interactive demo menu |
| `/tour` | Queue all 33 demos |
| `/demo photo` | Queue a specific demo by ID |
| `/report` | Show this chat's most recent result per demo |
| `/stop` | Cancel this chat's tour and clear its queued work |

The inline menu also runs each demo. The inline-button demo includes an
editable counter, an alert popup, a URL, and a clipboard button. Replies to
the force-reply prompt are acknowledged by their own handler class.

## Coverage

| Group | Demos |
| --- | --- |
| Text | HTML, spoilers, code and quotes; explicit Unicode entities; link previews; replies with quotes; editing; reactions; copy/forward; deletion |
| Media | Photo, spoiler photo, mixed photo/video album, video, looping animation, JSON document, audio with metadata, voice note, video note, WebP sticker, reuse via file ID |
| Interactive | Inline buttons, reply keyboard/removal, force reply, multiple-choice poll, quiz, dice/darts, typing action |
| Places and contacts | Fixed location, venue, live location edit/stop, fictional contact card |
| Latest API | Typed rich message blocks, streaming drafts, styled/disabled buttons |

This is a broad private-chat showcase, not every Bot API endpoint. Payments,
paid broadcasts, administration of groups, account/business operations,
ephemeral workflows, and Mini Apps need their own configuration or context
and are not part of the tour. New API methods and rendering can depend on
the Telegram server, client, or bot capabilities; failures are recorded and
do not abort the remaining demos.

The contact uses a fictional North American example number. Locations are
fixed sample points at Tokyo Station. All images, speech, and video are
original fixtures bundled in `assets/`; there are no remote media URLs.
To recreate fixtures, install Pillow, ffmpeg, espeak-ng, and fontconfig, then
run `python3 examples/showcase_bot/tools/make_assets.py`.

## Structure

```text
app/main.cpp            # Credentials, identity, lifecycle, polling
showcase/
  catalog.*             # Demo registry and generated menu
  service.*             # Per-chat sessions, queues, workers, cancellation, reports
  handlers.*            # Commands, callbacks, and force-reply handling
  module.*              # Registration and private-chat middleware
demos/                  # Typed Bot API examples grouped by feature
assets/                 # Original media fixtures
tests/                  # Offline payload and access-control tests
```

The worker receives no `Context` or `Next` references. Its API client supports
independent concurrent curl handles. Each chat owns its worker, queue,
cancellation, and report. Each queue holds at most three jobs and rejects
duplicate active/queued jobs. Up to eight chat workers can be live at once;
idle sessions are evicted to admit new users. If all workers are busy, another
user gets a short busy response and can retry. Counter state is keyed by chat
and message, so matching message IDs in different chats remain independent.
Calls are paced, most demo messages are silent, and network failures do not
trigger automatic replay of sends. Cancellation takes effect between HTTP
calls and interrupts waits. A live-location demo stops sharing at completion;
if cancelled early, its initial sharing period expires after 60 seconds.

Results are written atomically to `build/showcase_reports/<chat_id>.json` and
printed as JSON lines; reports are restored when sessions are recreated.
`TGPP_SHOWCASE_ASSETS` and `TGPP_SHOWCASE_REPORT_DIR` override the paths.
The previous single-chat report is migrated to the startup chat's file once.
The report contains demo IDs, titles, results, and API errors, without tokens
or message contents. `/report` is delivered to the chat that requested it.

Offline tests exercise every demo's generated request, named multipart uploads,
mixed albums, Unicode entity offsets, file reuse, rich blocks, access for
multiple users, isolated reports/counters/cancellation, worker limits, and
callback behavior. Live results remain separate from those tests.
