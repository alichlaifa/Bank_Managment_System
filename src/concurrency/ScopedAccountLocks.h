#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "concurrency/ScopedAccountLock.h"
#include "domain/Types.h"

namespace bank {

class AccountLockManager;

// RAII guard for two account mutexes, always acquired in ascending AccountId
// order (deadlock avoidance, see docs/concurrency/decisions.md). If
// first == second, exactly one mutex is held. Non-copyable, non-movable: the
// lock order must not be disturbed by moves. Destruction releases the
// higher-id lock first (second_ is declared after first_, so it is destroyed
// first), i.e. locks are released in reverse acquisition order.
class ScopedAccountLocks {
public:
    ScopedAccountLocks(AccountLockManager& manager, AccountId first, AccountId second);
    ~ScopedAccountLocks() = default;

    ScopedAccountLocks(const ScopedAccountLocks&) = delete;
    ScopedAccountLocks& operator=(const ScopedAccountLocks&) = delete;
    ScopedAccountLocks(ScopedAccountLocks&&) = delete;
    ScopedAccountLocks& operator=(ScopedAccountLocks&&) = delete;

    // Sorted ascending: ids()[0] <= ids()[1]. When size() == 1 both entries
    // are the same id.
    [[nodiscard]] const std::array<AccountId, 2>& ids() const noexcept { return ids_; }
    [[nodiscard]] std::size_t size() const noexcept { return second_.has_value() ? 2u : 1u; }

private:
    std::array<AccountId, 2> ids_{};
    std::optional<ScopedAccountLock> first_;   // lower id
    std::optional<ScopedAccountLock> second_;  // higher id, absent when ids equal
};

} // namespace bank
