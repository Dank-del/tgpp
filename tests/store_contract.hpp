#pragma once
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <stdexcept>
#include <tgpp/ext/stores.hpp>

// Reuse for developer-supplied distributed adapters. Run against an empty store
// using a controllable clock; each backend must satisfy the same fencing/CAS rules.
namespace store_contract {
inline void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

template <class T> T run(tgpp::Task<T> task) {
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(task), asio::use_future);
    io.run();
    return future.get();
}

inline void
verify(tgpp::ext::SessionStore& sessions, tgpp::ext::UpdateStore& updates, std::int64_t& now) {
    using namespace tgpp;
    using namespace tgpp::ext;
    using namespace std::chrono_literals;
    SessionKey key{"bot", "chat"};
    require(!run(sessions.load(key)), "new session must be absent");
    auto version = run(sessions.compare_exchange(key, 0, Json{{"step", 1}}, now + 100));
    require(version && *version == 1, "initial CAS must create version one");
    require(!run(sessions.compare_exchange(key, 0, Json{})), "stale create must fail");
    auto record = run(sessions.load(key));
    require(record && record->value.at("step") == 1, "session value roundtrip");
    now += 100;
    require(!run(sessions.load(key)), "expired session must be absent");
    auto new_version = run(sessions.compare_exchange(key, 0, Json{{"step", 2}}));
    require(new_version && *new_version > *version, "expiry must not permit version ABA");
    require(!run(sessions.compare_exchange(key, *version, Json{})),
            "stale session writer must fail");
    require(!run(updates.checkpoint("bot")), "new checkpoint absent");
    std::vector<InboxUpdate> batch{{1, "a", Json{{"update_id", 1}}},
                                   {2, "a", Json{{"update_id", 2}}},
                                   {3, "b", Json{{"update_id", 3}}}};
    require(run(updates.enqueue("bot", batch, 4)) == 3, "batch enqueue count");
    require(run(updates.enqueue("bot", batch, 4)) == 0, "duplicate batch must be ignored");
}

inline void verify_inbox(tgpp::ext::UpdateStore& updates, std::int64_t& now) {
    using namespace tgpp;
    using namespace tgpp::ext;
    using namespace std::chrono_literals;
    std::vector<InboxUpdate> batch{{10, "a", Json{{"update_id", 10}}},
                                   {11, "a", Json{{"update_id", 11}}},
                                   {12, "b", Json{{"update_id", 12}}}};
    require(run(updates.enqueue("inbox", batch, 13)) == 3, "batch enqueue count");
    require(run(updates.enqueue("inbox", batch, 13)) == 0, "duplicate batch must be ignored");
    require(run(updates.checkpoint("inbox")) == 13, "batch checkpoint");
    require(run(updates.enqueue("inbox", {}, 12)) == 0, "empty older checkpoint accepted");
    require(run(updates.checkpoint("inbox")) == 13, "checkpoint cannot regress");
    bool rejected = false;
    try {
        run(updates.enqueue("invalid", batch, 10));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && !run(updates.checkpoint("invalid")),
            "invalid batch must not partially commit");
    auto first = run(updates.claim("inbox", "one", 100ms));
    require(first && first->update.update_id == 10, "earliest key claim");
    auto other = run(updates.claim("inbox", "two", 100ms));
    require(other && other->update.update_id == 12, "different key must progress");
    require(!run(updates.claim("inbox", "three", 100ms)), "same key must remain ordered");
    require(run(updates.renew(*first, 200ms)), "active lease renewal");
    now += 100;
    require(!run(updates.complete(*other)), "expired completion must be fenced");
    auto replay = run(updates.claim("inbox", "three", 100ms));
    require(replay && replay->update.update_id == 12 &&
                replay->fencing_token > other->fencing_token,
            "expired lease reclaimed with fence");
    require(!run(updates.fail(*other, "stale")), "stale failure must be fenced");
    require(run(updates.complete(*replay)), "current owner completes");
    require(run(updates.fail(*first, "try again")), "first attempt fails");
    for (int attempt = 2; attempt <= 5; ++attempt) {
        auto claim = run(updates.claim("inbox", "one", 100ms));
        require(claim && claim->attempts == static_cast<unsigned>(attempt), "attempt count");
        require(run(updates.fail(*claim, "failure")), "failure recorded");
    }
    require(!run(updates.claim("inbox", "two", 100ms)), "fifth failure blocks key");
    require(run(updates.requeue("inbox", 10)), "explicit requeue");
    auto retry = run(updates.claim("inbox", "two", 100ms));
    require(retry && retry->attempts == 1, "requeue resets attempts");
    require(run(updates.complete(*retry)), "requeued update completes");
    auto next = run(updates.claim("inbox", "two", 100ms));
    require(next && next->update.update_id == 11, "next update unblocks");
    for (int attempt = 1; attempt <= 5; ++attempt) {
        require(run(updates.fail(*next, "skip me")), "fail for skip");
        if (attempt < 5) {
            next = run(updates.claim("inbox", "two", 100ms));
        }
    }
    require(run(updates.skip("inbox", 11)), "explicit skip");
    require(!run(updates.claim("inbox", "two", 100ms)), "all updates completed");
    require(run(updates.enqueue("inbox", batch, 13)) == 0, "completed dedup tombstones retained");
}

inline void verify_competing(tgpp::ext::SessionStore& sessions, tgpp::ext::UpdateStore& updates) {
    using namespace tgpp;
    using namespace tgpp::ext;
    using namespace std::chrono_literals;
    SessionKey key{"race", "session"};
    auto version = run(sessions.compare_exchange(key, 0, Json::object()));
    auto first = std::async(std::launch::async, [&] {
        return run(sessions.compare_exchange(key, *version, Json{{"writer", 1}}));
    });
    auto second = std::async(std::launch::async, [&] {
        return run(sessions.compare_exchange(key, *version, Json{{"writer", 2}}));
    });
    auto a = first.get();
    auto b = second.get();
    require(a.has_value() != b.has_value(), "exactly one competing CAS succeeds");
    std::vector<InboxUpdate> batch{{1, "same", Json{{"update_id", 1}}}};
    run(updates.enqueue("race", batch, 2));
    auto claimant1 =
        std::async(std::launch::async, [&] { return run(updates.claim("race", "one", 100ms)); });
    auto claimant2 =
        std::async(std::launch::async, [&] { return run(updates.claim("race", "two", 100ms)); });
    auto c = claimant1.get();
    auto d = claimant2.get();
    require(c.has_value() != d.has_value(), "exactly one competing claimant succeeds");
}

inline void verify_session_fence(tgpp::ext::SessionStore& sessions,
                                 tgpp::ext::UpdateStore& updates,
                                 std::int64_t& now) {
    using namespace tgpp;
    using namespace tgpp::ext;
    using namespace std::chrono_literals;
    std::vector<InboxUpdate> batch{{1, "chat", Json{{"update_id", 1}}}};
    run(updates.enqueue("fenced", batch, 2));
    auto first = run(updates.claim("fenced", "old", 100ms));
    SessionKey key{"fenced", "chat"};
    SessionFence stale{first->bot, first->update.update_id, first->owner, first->fencing_token};
    auto version = run(sessions.compare_exchange(key, 0, Json{{"step", 1}}, {}, stale));
    require(version.has_value(), "live lease can save session");
    now += 101;
    auto next = run(updates.claim("fenced", "new", 100ms));
    require(!run(sessions.compare_exchange(key, *version, Json{{"step", 2}}, {}, stale)),
            "stale lease cannot save session even with matching version");
    SessionFence live{next->bot, next->update.update_id, next->owner, next->fencing_token};
    require(run(sessions.compare_exchange(key, *version, Json{{"step", 3}}, {}, live)).has_value(),
            "new owner can save session");
}

} // namespace store_contract
