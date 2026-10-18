#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "concurrency/ScopedAccountLock.h"
#include "concurrency/ScopedAccountLocks.h"
#include "domain/Types.h"

namespace bank {

// Owns one std::mutex per AccountId, created on first use. The mutexes live
// here, never inside Account (a value type that is copied on every load).
// Non-copyable, non-movable.
class AccountLockManager {
public:
    AccountLockManager() = default;
    ~AccountLockManager() = default;

    AccountLockManager(const AccountLockManager&) = delete;
    AccountLockManager& operator=(const AccountLockManager&) = delete;
    AccountLockManager(AccountLockManager&&) = delete;
    AccountLockManager& operator=(AccountLockManager&&) = delete;

    [[nodiscard]] ScopedAccountLock lock(AccountId id);

    // Locks both in ascending id order (see ScopedAccountLocks).
    [[nodiscard]] ScopedAccountLocks lock(AccountId first, AccountId second);

    // Number of mutexes created so far (test/diagnostic use).
    [[nodiscard]] std::size_t mutex_count() const;

private:
    friend class ScopedAccountLock;
    friend class ScopedAccountLocks;

    // Returns the mutex for `id`, creating it if absent. map_mutex_ is
    // released before returning so the caller can lock the account mutex.
    [[nodiscard]] std::mutex& mutex_for(AccountId id);

    mutable std::mutex map_mutex_;
    std::unordered_map<AccountId, std::unique_ptr<std::mutex>> mutexes_;
};

} // namespace bank
