#pragma once
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <tgpp/async.hpp>
#include <vector>

namespace tgpp::ext {
using StoreClock = std::function<std::int64_t()>;
std::int64_t store_time_ms();

struct SessionKey {
    std::string bot;
    std::string key;
};

struct SessionRecord {
    Json value;
    std::uint64_t version = 0;
    std::optional<std::int64_t> expires_at_ms;
};

struct SessionFence {
    std::string bot;
    std::int64_t update_id = 0;
    std::string owner;
    std::uint64_t token = 0;
};

class SessionStore {
  public:
    virtual ~SessionStore() = default;
    virtual Task<std::optional<SessionRecord>> load(SessionKey key) = 0;
    // expected_version=0 means absent/expired. Versions never repeat for a key.
    // When supplied, fence must be checked atomically against the authoritative
    // inbox lease. Stale ownership or a revision conflict returns nullopt.
    virtual Task<std::optional<std::uint64_t>>
    compare_exchange(SessionKey key,
                     std::uint64_t expected_version,
                     Json value,
                     std::optional<std::int64_t> expires_at_ms = {},
                     std::optional<SessionFence> fence = {}) = 0;
};

struct InboxUpdate {
    std::int64_t update_id = 0;
    std::string ordering_key;
    Json payload;
};

struct UpdateClaim {
    std::string bot;
    InboxUpdate update;
    std::string owner;
    std::uint64_t fencing_token = 0;
    std::uint32_t attempts = 0;
    std::int64_t lease_until_ms = 0;
};

class UpdateStore {
  public:
    virtual ~UpdateStore() = default;
    // Inserts and checkpoint advance are atomic. Duplicates do not change payload/order.
    virtual Task<std::size_t> enqueue(std::string bot,
                                      std::vector<InboxUpdate> updates,
                                      std::optional<std::int64_t> next_offset = {}) = 0;
    virtual Task<std::optional<std::int64_t>> checkpoint(std::string bot) = 0;
    // Only the earliest unfinished update per key can be claimed. Failed keys block.
    virtual Task<std::optional<UpdateClaim>>
    claim(std::string bot, std::string owner, std::chrono::milliseconds lease) = 0;
    virtual Task<bool> renew(UpdateClaim claim, std::chrono::milliseconds lease) = 0;
    virtual Task<bool> complete(UpdateClaim claim) = 0;
    // Fifth failed/expired attempt blocks the key until explicit requeue or skip.
    virtual Task<bool> fail(UpdateClaim claim, std::string reason) = 0;
    virtual Task<bool> requeue(std::string bot, std::int64_t update_id) = 0;
    virtual Task<bool> skip(std::string bot, std::int64_t update_id) = 0;
};

class MemoryStores final : public SessionStore, public UpdateStore {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    explicit MemoryStores(StoreClock clock = {});
    ~MemoryStores();
    Task<std::optional<SessionRecord>> load(SessionKey key) override;
    Task<std::optional<std::uint64_t>>
    compare_exchange(SessionKey key,
                     std::uint64_t expected_version,
                     Json value,
                     std::optional<std::int64_t> expires_at_ms = {},
                     std::optional<SessionFence> fence = {}) override;
    Task<std::size_t> enqueue(std::string bot,
                              std::vector<InboxUpdate> updates,
                              std::optional<std::int64_t> next_offset = {}) override;
    Task<std::optional<std::int64_t>> checkpoint(std::string bot) override;
    Task<std::optional<UpdateClaim>>
    claim(std::string bot, std::string owner, std::chrono::milliseconds lease) override;
    Task<bool> renew(UpdateClaim claim, std::chrono::milliseconds lease) override;
    Task<bool> complete(UpdateClaim claim) override;
    Task<bool> fail(UpdateClaim claim, std::string reason) override;
    Task<bool> requeue(std::string bot, std::int64_t update_id) override;
    Task<bool> skip(std::string bot, std::int64_t update_id) override;
};
} // namespace tgpp::ext
