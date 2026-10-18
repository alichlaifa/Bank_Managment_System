#include "concurrency/AccountLockManager.h"

#include <utility>

#include "concurrency/ScopedAccountLocks.h"

namespace bank {

ScopedAccountLock::ScopedAccountLock(AccountLockManager& manager, AccountId id)
    : ScopedAccountLock{manager.mutex_for(id), id}
{
}

ScopedAccountLocks::ScopedAccountLocks(AccountLockManager& manager, AccountId first,
                                       AccountId second)
{
    if (second < first) {
        std::swap(first, second);
    }
    ids_ = {first, second};
    // Acquisition order: lower id first (first_), then higher (second_).
    // Destruction is the reverse, so no lock-order inversion is possible.
    first_.emplace(manager, first);
    if (second != first) {
        second_.emplace(manager, second);
    }
}

std::mutex& AccountLockManager::mutex_for(AccountId id)
{
    // The lookup/insert holds map_mutex_ only for the duration of this
    // function; the account mutex itself is locked by the delegating
    // ScopedAccountLock constructor after map_mutex_ has been released.
    std::lock_guard<std::mutex> guard{map_mutex_};
    auto& slot = mutexes_[id];
    if (slot == nullptr) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}

ScopedAccountLock AccountLockManager::lock(AccountId id)
{
    return ScopedAccountLock{*this, id};
}

ScopedAccountLocks AccountLockManager::lock(AccountId first, AccountId second)
{
    return ScopedAccountLocks{*this, first, second};
}

std::size_t AccountLockManager::mutex_count() const
{
    std::lock_guard<std::mutex> guard{map_mutex_};
    return mutexes_.size();
}

} // namespace bank
