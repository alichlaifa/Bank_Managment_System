#pragma once

#include <mutex>

#include "domain/Types.h"

namespace bank {

class AccountLockManager;

// RAII guard for exactly one account mutex. Locks on construction, unlocks on
// destruction. Move-only: a move transfers ownership of the lock and leaves
// the source guard holding nothing.
class ScopedAccountLock {
public:
    // Standalone form: locks an arbitrary mutex; carries AccountId{} (0).
    explicit ScopedAccountLock(std::mutex& mutex)
        : ScopedAccountLock{mutex, AccountId{}}
    {
    }

    // Manager form: locks the manager's mutex for `id` (creating it on
    // demand under the manager's map mutex). Defined in
    // AccountLockManager.cpp.
    ScopedAccountLock(AccountLockManager& manager, AccountId id);

    ~ScopedAccountLock()
    {
        if (mutex_ != nullptr) {
            mutex_->unlock();
        }
    }

    ScopedAccountLock(const ScopedAccountLock&) = delete;
    ScopedAccountLock& operator=(const ScopedAccountLock&) = delete;

    ScopedAccountLock(ScopedAccountLock&& other) noexcept
        : mutex_{other.mutex_}, id_{other.id_}
    {
        other.mutex_ = nullptr;
    }

    ScopedAccountLock& operator=(ScopedAccountLock&& other) noexcept
    {
        if (this != &other) {
            if (mutex_ != nullptr) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            id_ = other.id_;
            other.mutex_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] AccountId id() const noexcept { return id_; }

private:
    friend class AccountLockManager;
    friend class ScopedAccountLocks;

    // Internal form used by AccountLockManager::lock: caller hands over an
    // already-looked-up mutex plus the id it belongs to. Locks immediately.
    ScopedAccountLock(std::mutex& mutex, AccountId id) : mutex_{&mutex}, id_{id}
    {
        mutex_->lock();
    }

    std::mutex* mutex_{nullptr};
    AccountId id_{};
};

} // namespace bank
