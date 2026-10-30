#include <limits>
#include <map>
#include <mutex>
#include <tgpp/ext/stores.hpp>

namespace tgpp::ext {
std::int64_t store_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

namespace {
std::int64_t until(std::int64_t now, std::chrono::milliseconds lease) {
    if (lease.count() <= 0 || now > std::numeric_limits<std::int64_t>::max() - lease.count()) {
        throw std::invalid_argument("invalid store lease duration");
    }
    return now + lease.count();
}

void validate_batch(const std::string& bot,
                    const std::vector<InboxUpdate>& updates,
                    std::optional<std::int64_t> offset) {
    if (bot.empty() || (offset && *offset < 0)) {
        throw std::invalid_argument("invalid inbox identity or offset");
    }
    for (const auto& update : updates) {
        if (update.update_id < 0 || update.update_id == std::numeric_limits<std::int64_t>::max() ||
            (offset && update.update_id >= *offset)) {
            throw std::invalid_argument("invalid inbox update identifier or checkpoint");
        }
    }
}
} // namespace

struct MemoryStores::Impl {
    struct Row {
        InboxUpdate update;
        std::string owner, error;
        std::uint64_t token = 0;
        std::uint32_t attempts = 0;
        std::int64_t until = 0;
        bool done = false, failed = false;
    };

    StoreClock clock;
    std::mutex mutex;
    std::map<std::pair<std::string, std::string>, SessionRecord> sessions;
    std::map<std::pair<std::string, std::int64_t>, Row> updates;
    std::map<std::string, std::int64_t> offsets;

    explicit Impl(StoreClock value) : clock(value ? std::move(value) : store_time_ms) {}

    bool valid(const UpdateClaim& claim, Row& row) {
        return !row.done && !row.failed && row.owner == claim.owner &&
               row.token == claim.fencing_token && !row.owner.empty() && row.until > clock();
    }
};

MemoryStores::MemoryStores(StoreClock clock) : impl_(std::make_shared<Impl>(std::move(clock))) {}

MemoryStores::~MemoryStores() = default;

Task<std::optional<SessionRecord>> MemoryStores::load(SessionKey key) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->sessions.find({key.bot, key.key});
    if (found == impl->sessions.end() ||
        (found->second.expires_at_ms && *found->second.expires_at_ms <= impl->clock())) {
        co_return std::nullopt;
    }
    co_return found->second;
}

Task<std::optional<std::uint64_t>>
MemoryStores::compare_exchange(SessionKey key,
                               std::uint64_t expected,
                               Json value,
                               std::optional<std::int64_t> expiry,
                               std::optional<SessionFence> fence) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    if (fence) {
        auto entry = impl->updates.find({fence->bot, fence->update_id});
        if (fence->bot != key.bot || entry == impl->updates.end() || entry->second.done ||
            entry->second.failed || entry->second.owner != fence->owner ||
            entry->second.token != fence->token || entry->second.owner.empty() ||
            entry->second.until <= impl->clock()) {
            co_return std::nullopt;
        }
    }
    auto found = impl->sessions.find({key.bot, key.key});
    const bool absent =
        found == impl->sessions.end() ||
        (found->second.expires_at_ms && *found->second.expires_at_ms <= impl->clock());
    const auto version = absent ? 0 : found->second.version;
    if (version != expected) {
        co_return std::nullopt;
    }
    auto next = found == impl->sessions.end() ? 1 : found->second.version + 1;
    if (next == 0) {
        throw std::overflow_error("session version exhausted");
    }
    impl->sessions[{key.bot, key.key}] = {std::move(value), next, expiry};
    co_return next;
}

Task<std::size_t> MemoryStores::enqueue(std::string bot,
                                        std::vector<InboxUpdate> updates,
                                        std::optional<std::int64_t> offset) {
    validate_batch(bot, updates, offset);
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    std::size_t count = 0;
    for (auto& update : updates) {
        const auto id = update.update_id;
        count +=
            impl->updates.emplace(std::make_pair(bot, id), Impl::Row{std::move(update)}).second;
    }
    if (offset) {
        auto found = impl->offsets.find(bot);
        if (found == impl->offsets.end() || *offset > found->second) {
            impl->offsets[bot] = *offset;
        }
    }
    co_return count;
}

Task<std::optional<std::int64_t>> MemoryStores::checkpoint(std::string bot) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->offsets.find(bot);
    if (found == impl->offsets.end()) {
        co_return std::nullopt;
    }
    co_return found->second;
}

Task<std::optional<UpdateClaim>>
MemoryStores::claim(std::string bot, std::string owner, std::chrono::milliseconds lease) {
    if (owner.empty()) {
        throw std::invalid_argument("empty lease owner");
    }
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    const auto now = impl->clock(), deadline = until(now, lease);
    std::map<std::string, bool> seen;
    for (auto& [key, row] : impl->updates) {
        if (key.first != bot || row.done) {
            continue;
        }
        if (!seen.emplace(row.update.ordering_key, true).second) {
            continue;
        }
        if (row.failed || row.until > now) {
            continue;
        }
        if (row.attempts >= 5) {
            row.failed = true;
            row.error = "lease expired after fifth attempt";
            continue;
        }
        row.owner = owner;
        row.until = deadline;
        ++row.token;
        ++row.attempts;
        co_return UpdateClaim{bot, row.update, owner, row.token, row.attempts, deadline};
    }
    co_return std::nullopt;
}

Task<bool> MemoryStores::renew(UpdateClaim claim, std::chrono::milliseconds lease) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto deadline = until(impl->clock(), lease);
    auto found = impl->updates.find({claim.bot, claim.update.update_id});
    if (found == impl->updates.end() || !impl->valid(claim, found->second)) {
        co_return false;
    }
    found->second.until = deadline;
    co_return true;
}

Task<bool> MemoryStores::complete(UpdateClaim claim) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->updates.find({claim.bot, claim.update.update_id});
    if (found == impl->updates.end() || !impl->valid(claim, found->second)) {
        co_return false;
    }
    found->second.done = true;
    found->second.owner.clear();
    co_return true;
}

Task<bool> MemoryStores::fail(UpdateClaim claim, std::string reason) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->updates.find({claim.bot, claim.update.update_id});
    if (found == impl->updates.end() || !impl->valid(claim, found->second)) {
        co_return false;
    }
    auto& row = found->second;
    row.owner.clear();
    row.until = 0;
    row.error = std::move(reason);
    row.failed = row.attempts >= 5;
    co_return true;
}

Task<bool> MemoryStores::requeue(std::string bot, std::int64_t id) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->updates.find({bot, id});
    if (found == impl->updates.end() || !found->second.failed) {
        co_return false;
    }
    auto& row = found->second;
    row.failed = false;
    row.attempts = 0;
    row.until = 0;
    row.owner.clear();
    ++row.token;
    co_return true;
}

Task<bool> MemoryStores::skip(std::string bot, std::int64_t id) {
    auto impl = impl_;
    std::lock_guard lock(impl->mutex);
    auto found = impl->updates.find({bot, id});
    if (found == impl->updates.end() || !found->second.failed) {
        co_return false;
    }
    found->second.done = true;
    found->second.failed = false;
    ++found->second.token;
    co_return true;
}
} // namespace tgpp::ext
