#include <asio/associated_executor.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>
#include <atomic>
#include <limits>
#include <sqlite3.h>
#include <tgpp/ext/sqlite_stores.hpp>

namespace tgpp::ext {
namespace {
class Statement {
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;

  public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    ~Statement() {
        sqlite3_finalize(stmt_);
    }

    void text(int i, const std::string& value) {
        if (sqlite3_bind_text(
                stmt_, i, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
    }

    void integer(int i, std::int64_t value) {
        sqlite3_bind_int64(stmt_, i, value);
    }

    void null(int i) {
        sqlite3_bind_null(stmt_, i);
    }

    bool step() {
        int code = sqlite3_step(stmt_);
        if (code == SQLITE_ROW) {
            return true;
        }
        if (code != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(db_));
        }
        return false;
    }

    std::int64_t integer(int i) {
        return sqlite3_column_int64(stmt_, i);
    }

    bool is_null(int i) {
        return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
    }

    std::string text(int i) {
        const auto* value = sqlite3_column_text(stmt_, i);
        return value ? std::string(reinterpret_cast<const char*>(value),
                                   sqlite3_column_bytes(stmt_, i))
                     : std::string{};
    }
};

void exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "SQLite failure";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

class Transaction {
    sqlite3* db_;
    bool committed_ = false;

  public:
    explicit Transaction(sqlite3* db) : db_(db) {
        exec(db_, "BEGIN IMMEDIATE");
    }

    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void commit() {
        exec(db_, "COMMIT");
        committed_ = true;
    }
};

std::int64_t deadline(std::int64_t now, std::chrono::milliseconds lease) {
    if (lease.count() <= 0 || now > std::numeric_limits<std::int64_t>::max() - lease.count()) {
        throw std::invalid_argument("invalid store lease duration");
    }
    return now + lease.count();
}

void bind_claim(Statement& s, const UpdateClaim& claim, std::int64_t now, int index = 1) {
    s.text(index, claim.bot);
    s.integer(index + 1, claim.update.update_id);
    s.text(index + 2, claim.owner);
    s.integer(index + 3, static_cast<std::int64_t>(claim.fencing_token));
    s.integer(index + 4, now);
}
} // namespace

struct SqliteStores::Impl : std::enable_shared_from_this<Impl> {
    sqlite3* db = nullptr;
    StoreClock clock;
    std::size_t capacity;
    std::atomic<std::size_t> pending{0};
    asio::thread_pool worker{1};

    Impl(std::string path, StoreClock value, std::size_t bound)
        : clock(value ? std::move(value) : store_time_ms), capacity(bound) {
        if (!capacity) {
            throw std::invalid_argument("SQLite queue capacity must be positive");
        }
        if (sqlite3_open_v2(path.c_str(),
                            &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            std::string message = db ? sqlite3_errmsg(db) : "cannot allocate SQLite database";
            if (db) {
                sqlite3_close(db);
            }
            db = nullptr;
            throw std::runtime_error(message);
        }
        try {
            sqlite3_busy_timeout(db, 5000);
            exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
            Statement version(db, "PRAGMA user_version");
            version.step();
            if (version.integer(0) > 1) {
                throw std::runtime_error("unsupported SQLite store schema version");
            }
            exec(db,
                 "CREATE TABLE IF NOT EXISTS sessions(bot TEXT NOT NULL,k TEXT NOT NULL,value TEXT "
                 "NOT NULL,version INTEGER NOT NULL,expiry INTEGER,PRIMARY KEY(bot,k));"
                 "CREATE TABLE IF NOT EXISTS inbox(bot TEXT NOT NULL,id INTEGER NOT NULL,k TEXT "
                 "NOT NULL,payload TEXT NOT NULL,owner TEXT NOT NULL DEFAULT '',token INTEGER NOT "
                 "NULL DEFAULT 0,attempts INTEGER NOT NULL DEFAULT 0,lease INTEGER NOT NULL "
                 "DEFAULT 0,state INTEGER NOT NULL DEFAULT 0,error TEXT NOT NULL DEFAULT "
                 "'',PRIMARY KEY(bot,id));"
                 "CREATE INDEX IF NOT EXISTS inbox_order ON inbox(bot,k,id);"
                 "CREATE TABLE IF NOT EXISTS checkpoints(bot TEXT PRIMARY KEY,offset INTEGER NOT "
                 "NULL);"
                 "PRAGMA user_version=1;");
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl() {
        worker.join();
        if (db) {
            sqlite3_close(db);
        }
    }

    template <class F> auto run(F fn) -> Task<std::invoke_result_t<F, sqlite3*>> {
        using R = std::invoke_result_t<F, sqlite3*>;
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, R)>(
            [self, fn = std::move(fn)](auto handler) mutable {
                // Completion on another executor can destroy this initiating
                // coroutine (and its lambda captures) before post() returns.
                // Only ordinary locals may be used once completion is published.
                auto owner = self;
                auto function = std::move(fn);
                auto executor = asio::get_associated_executor(handler);
                auto completion = std::make_shared<decltype(handler)>(std::move(handler));
                auto work = std::make_shared<asio::executor_work_guard<asio::any_io_executor>>(
                    asio::make_work_guard(executor));
                const auto count = owner->pending.fetch_add(1);
                if (count >= owner->capacity) {
                    owner->pending.fetch_sub(1);
                    asio::post(executor, [owner, completion, work]() mutable {
                        (*completion)(std::make_exception_ptr(OverloadError()), R{});
                        work->reset();
                    });
                    return;
                }
                try {
                    asio::post(owner->worker,
                               [owner,
                                executor,
                                function = std::move(function),
                                completion,
                                work]() mutable {
                                   R result{};
                                   std::exception_ptr error;
                                   try {
                                       result = function(owner->db);
                                   } catch (...) {
                                       error = std::current_exception();
                                   }
                                   owner->pending.fetch_sub(1);
                                   // These copies outlive a concurrently invoked completion;
                                   // do not read the queued worker lambda after publishing it.
                                   auto keep_owner = owner;
                                   auto finish = completion;
                                   auto keep_work = work;
                                   asio::post(executor,
                                              [keep_owner,
                                               finish,
                                               keep_work,
                                               error,
                                               result = std::move(result)]() mutable {
                                                  // Handler invocation can destroy the awaiting
                                                  // frame. These are captures of the separately
                                                  // owned posted operation, never members of that
                                                  // coroutine frame.
                                                  (*finish)(error, std::move(result));
                                                  keep_work->reset();
                                              });
                               });
                } catch (...) {
                    owner->pending.fetch_sub(1);
                    throw;
                }
            },
            token);
    }
};

SqliteStores::SqliteStores(std::string path, StoreClock clock, std::size_t capacity)
    : impl_(std::make_shared<Impl>(std::move(path), std::move(clock), capacity)) {}

SqliteStores::~SqliteStores() {
    // Join while impl_ is still strongly owned by this external-thread object.
    // Otherwise a completion could release the caller's last reference while
    // the database worker is returning from post(), destroying its own pool.
    impl_->worker.join();
}

Task<std::optional<SessionRecord>> SqliteStores::load(SessionKey key) {
    auto p = impl_;
    co_return co_await p->run(
        [p, key = std::move(key)](sqlite3* db) -> std::optional<SessionRecord> {
            Statement s(db, "SELECT value,version,expiry FROM sessions WHERE bot=? AND k=?");
            s.text(1, key.bot);
            s.text(2, key.key);
            if (!s.step()) {
                return {};
            }
            std::optional<std::int64_t> expiry;
            if (!s.is_null(2)) {
                expiry = s.integer(2);
            }
            if (expiry && *expiry <= p->clock()) {
                return {};
            }
            return SessionRecord{
                Json::parse(s.text(0)), static_cast<std::uint64_t>(s.integer(1)), expiry};
        });
}

Task<std::optional<std::uint64_t>>
SqliteStores::compare_exchange(SessionKey key,
                               std::uint64_t expected,
                               Json value,
                               std::optional<std::int64_t> expiry,
                               std::optional<SessionFence> fence) {
    auto p = impl_;
    co_return co_await p->run([p,
                               key = std::move(key),
                               expected,
                               value = std::move(value),
                               expiry,
                               fence](sqlite3* db) -> std::optional<std::uint64_t> {
        Transaction tx(db);
        if (fence) {
            if (fence->bot != key.bot) {
                return {};
            }
            Statement lease(db,
                            "SELECT 1 FROM inbox WHERE bot=? AND id=? AND owner=? AND token=? "
                            "AND lease>? AND state=0");
            lease.text(1, fence->bot);
            lease.integer(2, fence->update_id);
            lease.text(3, fence->owner);
            lease.integer(4, static_cast<std::int64_t>(fence->token));
            lease.integer(5, p->clock());
            if (fence->owner.empty() || !lease.step()) {
                return {};
            }
        }
        std::int64_t previous = 0;
        bool absent = true;
        {
            Statement s(db, "SELECT version,expiry FROM sessions WHERE bot=? AND k=?");
            s.text(1, key.bot);
            s.text(2, key.key);
            if (s.step()) {
                previous = s.integer(0);
                absent = !s.is_null(1) && s.integer(1) <= p->clock();
            }
        }
        if ((absent ? 0 : static_cast<std::uint64_t>(previous)) != expected) {
            return {};
        }
        if (previous == std::numeric_limits<std::int64_t>::max()) {
            throw std::overflow_error("session version exhausted");
        }
        Statement s(
            db,
            "INSERT INTO sessions(bot,k,value,version,expiry) VALUES(?,?,?,?,?) ON CONFLICT(bot,k) "
            "DO UPDATE SET value=excluded.value,version=excluded.version,expiry=excluded.expiry");
        s.text(1, key.bot);
        s.text(2, key.key);
        s.text(3, value.dump());
        s.integer(4, previous + 1);
        if (expiry) {
            s.integer(5, *expiry);
        } else {
            s.null(5);
        }
        s.step();
        tx.commit();
        return static_cast<std::uint64_t>(previous + 1);
    });
}

Task<std::size_t> SqliteStores::enqueue(std::string bot,
                                        std::vector<InboxUpdate> updates,
                                        std::optional<std::int64_t> offset) {
    if (bot.empty() || (offset && *offset < 0)) {
        throw std::invalid_argument("invalid inbox identity or offset");
    }
    for (const auto& u : updates) {
        if (u.update_id < 0 || u.update_id == std::numeric_limits<std::int64_t>::max() ||
            (offset && u.update_id >= *offset)) {
            throw std::invalid_argument("invalid inbox update identifier or checkpoint");
        }
    }
    auto p = impl_;
    co_return co_await p->run(
        [bot = std::move(bot), updates = std::move(updates), offset](sqlite3* db) {
            Transaction tx(db);

            std::size_t count = 0;
            for (const auto& u : updates) {
                Statement s(db, "INSERT OR IGNORE INTO inbox(bot,id,k,payload) VALUES(?,?,?,?)");
                s.text(1, bot);
                s.integer(2, u.update_id);
                s.text(3, u.ordering_key);
                s.text(4, u.payload.dump());
                s.step();
                count += sqlite3_changes(db);
            }
            if (offset) {
                Statement s(db,
                            "INSERT INTO checkpoints(bot,offset) VALUES(?,?) ON CONFLICT(bot) DO "
                            "UPDATE SET offset=MAX(offset,excluded.offset)");
                s.text(1, bot);
                s.integer(2, *offset);
                s.step();
            }
            tx.commit();
            return count;
        });
}

Task<std::optional<std::int64_t>> SqliteStores::checkpoint(std::string bot) {
    auto p = impl_;
    co_return co_await p->run([bot = std::move(bot)](sqlite3* db) -> std::optional<std::int64_t> {
        Statement s(db, "SELECT offset FROM checkpoints WHERE bot=?");
        s.text(1, bot);
        if (s.step()) {
            return s.integer(0);
        }
        return {};
    });
}

Task<std::optional<UpdateClaim>>
SqliteStores::claim(std::string bot, std::string owner, std::chrono::milliseconds lease) {
    if (owner.empty()) {
        throw std::invalid_argument("empty lease owner");
    }
    auto p = impl_;
    co_return co_await p->run([p, bot = std::move(bot), owner = std::move(owner), lease](
                                  sqlite3* db) -> std::optional<UpdateClaim> {
        auto now = p->clock(), end = deadline(now, lease);
        Transaction tx(db);
        {
            Statement s(db,
                        "UPDATE inbox SET state=2,error='lease expired after fifth "
                        "attempt',owner='' WHERE bot=? AND state=0 AND attempts>=5 AND lease<=?");
            s.text(1, bot);
            s.integer(2, now);
            s.step();
        }
        std::optional<UpdateClaim> claim;
        {
            Statement s(db,
                        "SELECT i.id,i.k,i.payload,i.token,i.attempts FROM inbox i WHERE i.bot=? "
                        "AND i.state=0 AND i.lease<=? AND NOT EXISTS(SELECT 1 FROM inbox older "
                        "WHERE older.bot=i.bot AND older.k=i.k AND older.id<i.id AND "
                        "older.state<>1) ORDER BY i.id LIMIT 1");
            s.text(1, bot);
            s.integer(2, now);
            if (s.step()) {
                claim = UpdateClaim{bot,
                                    {s.integer(0), s.text(1), Json::parse(s.text(2))},
                                    owner,
                                    static_cast<std::uint64_t>(s.integer(3) + 1),
                                    static_cast<std::uint32_t>(s.integer(4) + 1),
                                    end};
            }
        }
        if (claim) {
            Statement s(db,
                        "UPDATE inbox SET owner=?,token=?,attempts=?,lease=? WHERE bot=? AND id=?");
            s.text(1, owner);
            s.integer(2, static_cast<std::int64_t>(claim->fencing_token));
            s.integer(3, claim->attempts);
            s.integer(4, end);
            s.text(5, bot);
            s.integer(6, claim->update.update_id);
            s.step();
        }
        tx.commit();
        return claim;
    });
}

Task<bool> SqliteStores::renew(UpdateClaim claim, std::chrono::milliseconds lease) {
    if (claim.owner.empty()) {
        co_return false;
    }
    auto p = impl_;
    co_return co_await p->run([p, claim = std::move(claim), lease](sqlite3* db) {
        auto now = p->clock(), end = deadline(now, lease);
        Statement s(db,
                    "UPDATE inbox SET lease=? WHERE bot=? AND id=? AND owner=? AND token=? AND "
                    "lease>? AND state=0");
        s.integer(1, end);
        bind_claim(s, claim, now, 2);
        s.step();
        return sqlite3_changes(db) != 0;
    });
}

Task<bool> SqliteStores::complete(UpdateClaim claim) {
    if (claim.owner.empty()) {
        co_return false;
    }
    auto p = impl_;
    co_return co_await p->run([p, claim = std::move(claim)](sqlite3* db) {
        Statement s(db,
                    "UPDATE inbox SET state=1,owner='' WHERE bot=? AND id=? AND owner=? AND "
                    "token=? AND lease>? AND state=0");
        bind_claim(s, claim, p->clock());
        s.step();
        return sqlite3_changes(db) != 0;
    });
}

Task<bool> SqliteStores::fail(UpdateClaim claim, std::string reason) {
    if (claim.owner.empty()) {
        co_return false;
    }
    auto p = impl_;
    co_return co_await p->run(
        [p, claim = std::move(claim), reason = std::move(reason)](sqlite3* db) {
            Statement s(db,
                        "UPDATE inbox SET state=CASE WHEN attempts>=5 THEN 2 ELSE 0 "
                        "END,owner='',lease=0,error=? WHERE bot=? AND id=? AND owner=? AND token=? "
                        "AND lease>? AND state=0");
            s.text(1, reason);
            bind_claim(s, claim, p->clock(), 2);
            s.step();
            return sqlite3_changes(db) != 0;
        });
}

Task<bool> SqliteStores::requeue(std::string bot, std::int64_t id) {
    auto p = impl_;
    co_return co_await p->run([bot = std::move(bot), id](sqlite3* db) {
        Statement s(db,
                    "UPDATE inbox SET state=0,attempts=0,owner='',lease=0,token=token+1 WHERE "
                    "bot=? AND id=? AND state=2");
        s.text(1, bot);
        s.integer(2, id);
        s.step();
        return sqlite3_changes(db) != 0;
    });
}

Task<bool> SqliteStores::skip(std::string bot, std::int64_t id) {
    auto p = impl_;
    co_return co_await p->run([bot = std::move(bot), id](sqlite3* db) {
        Statement s(
            db, "UPDATE inbox SET state=1,owner='',token=token+1 WHERE bot=? AND id=? AND state=2");
        s.text(1, bot);
        s.integer(2, id);
        s.step();
        return sqlite3_changes(db) != 0;
    });
}
} // namespace tgpp::ext
