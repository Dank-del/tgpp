#pragma once
#include "stores.hpp"

namespace tgpp::ext {
// One bot process per database. Operations execute on a dedicated bounded worker.
// The file persists inbox tombstones (deduplication), sessions, and checkpoints.
class SqliteStores final : public SessionStore, public UpdateStore {
    struct Impl;
    std::shared_ptr<Impl> impl_;

  public:
    explicit SqliteStores(std::string path,
                          StoreClock clock = {},
                          std::size_t queue_capacity = 1024);
    ~SqliteStores();
    SqliteStores(const SqliteStores&) = delete;
    SqliteStores& operator=(const SqliteStores&) = delete;
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
