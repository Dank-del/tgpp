# Sessions and durable updates

`tgpp::ext::SessionStore` and `UpdateStore` are coroutine interfaces. The built-in
`MemoryStores` implements both without persistence. Enable `TGPP_WITH_SQLITE`
and link `tgpp::sqlite` for `SqliteStores`; the database retains sessions, inbox
records, deduplication tombstones, and polling checkpoints across restarts.
SQLite operations run on one dedicated worker with a bounded queue (1,024 by
default), WAL journaling, and `synchronous=FULL`. Construct the store at startup;
opening and initializing its database is synchronous. Keep it alive until its
operations and application have finished. One bot process owns a SQLite database.

```cpp
#include <tgpp/ext/sqlite_stores.hpp>

tgpp::Task<void> edit_session(tgpp::ext::SessionStore& store) {
    tgpp::ext::SessionKey key{"my-bot-id", "chat:123"};
    auto old = co_await store.load(key);
    auto value = old ? old->value : tgpp::Json::object();
    value["step"] = "waiting_for_name";
    auto saved = co_await store.compare_exchange(
        key, old ? old->version : 0, std::move(value));
    if (!saved) throw std::runtime_error("session was changed concurrently");
}
```

Use a stable non-secret bot identity; never use the bot token as a database key.
Session expiry is an absolute Unix timestamp in milliseconds. Version zero means
absent or expired. Expiration does not reset the revision counter: an old writer
cannot overwrite a newly created session after expiry. A compare-and-swap conflict
requires application handling; blindly repeating business logic may duplicate
side effects.

## Durable ingestion and processing

`enqueue(bot, batch, next_offset)` commits an entire batch and its retrieval
checkpoint atomically. A duplicate `(bot, update_id)` is ignored, preserving the
original payload. Checkpoints only advance. The polling adapter should resume
from `checkpoint(bot)` and only acknowledge a fetched batch after `enqueue`
succeeds. A webhook adapter should return success after its enqueue succeeds;
repeated delivery of the same update is successful without duplicate insertion.

Each `InboxUpdate` contains an update ID, an application-defined ordering key,
and the raw update JSON. Use the same key calculation for all ingestion paths.
The first unfinished update for each key is eligible for claiming; unrelated
keys remain independent. A claim increments both its attempt counter and fencing
token. Its lease must be renewed before expiry when processing takes longer than
the initial lease. Completion, renewal, and failure require a matching live owner
and fencing token. A late worker cannot complete a reclaimed update.

After five failed or expired attempts, the update blocks its key. Operators can
inspect the persisted SQLite `inbox` table (`state=2`, with `error` and `attempts`)
and call `requeue(bot, id)` to reset attempts, or `skip(bot, id)` to release the
key. These methods affect blocked records only. Completed/skipped records remain
as deduplication tombstones; plan storage retention for long-lived deployments.
There is no automatic pruning because deleting a tombstone permits old deliveries
to be processed again.

When session and inbox ownership are coupled, pass a `SessionFence` (bot,
update ID, owner, fencing token) to `compare_exchange`. The built-in combined
stores check the live inbox lease in the same critical section/SQL transaction
as the session write. The async session middleware does this automatically for
durable claims. A custom SessionStore must implement the same atomic check
against its authoritative inbox; using unrelated backends cannot provide that
atomic guarantee.

Delivery is **at least once**. A process can send a Telegram message and fail
before recording completion. Session CAS, inbox completion, and an external API
side effect do not form a single transaction. Use application idempotency keys,
a transactional outbox in your own backend, or business-specific reconciliation
where repeated side effects matter.

## Developer-supplied distributed backends

Implement the two interfaces in `tgpp/ext/stores.hpp` using your existing
PostgreSQL, Redis, or other infrastructure. No distributed database dependency
is added by tgpp. Await database/network operations rather than blocking the
runtime executor. Store timestamps use a consistent authoritative clock; leases
must not depend on unsynchronized process-local wall clocks.

A distributed `UpdateStore` must atomically provide:

- Deduplicated batch insertion and monotonic checkpoint advancement.
- An exclusive claim for the earliest unfinished update per ordering key.
- Increasing fencing tokens when ownership changes, and expiry checks for every
  renewal, completion, and failure.
- A blocked key after the fifth attempt, with explicit requeue/skip recovery.

A distributed `SessionStore` must atomically compare revisions and preserve
revision monotonicity across expiry. Use a transaction or backend-native script;
separate reads and writes do not implement compare-and-swap.

Run the reusable `tests/store_contract.hpp` checks against an empty adapter with
an injectable clock. Also run real multi-process tests for competing claims,
network partitions, stale owners, and database transaction rollback. The built-in
contract suite tests semantics in one process; it does not certify a remote
backend's distributed consistency.

For one bot token, elect or otherwise coordinate **one polling ingestor**. Multiple
processes may consume a shared inbox implemented by a distributed adapter.
Webhook replicas may enqueue into that same store. SQLite is not the distributed
adapter. Never run independent long pollers on every replica.

Ordering applies to updates already present in the shared inbox. Parallel webhook
requests can arrive out of order; a store cannot anticipate a missing lower update
ID. Use Telegram webhook `max_connections=1` when delivery order is required,
or implement an ingestion sequencer in your distributed adapter.
