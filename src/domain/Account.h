#pragma once

#include "domain/Types.h"
#include "domain/Money.h"

#include <cstdint>
#include <stdexcept>

namespace bank {

// An account keeps an optimistic version counter: every mutation bumps it, so
// the concurrent processor can detect a lost update when persisting. The
// balance invariant (never negative) is structural and enforced at all entry
// points, not assumed.
class Account {
public:
    Account(AccountId id, CustomerId owner_id, Money balance, AccountStatus status,
            uint64_t version)
        : id_{id}, owner_id_{owner_id}, balance_{balance}, status_{status}, version_{version}
    {
    }

    [[nodiscard]] AccountId id() const noexcept { return id_; }
    [[nodiscard]] CustomerId owner_id() const noexcept { return owner_id_; }
    [[nodiscard]] Money balance() const noexcept { return balance_; }
    [[nodiscard]] AccountStatus status() const noexcept { return status_; }
    [[nodiscard]] uint64_t version() const noexcept { return version_; }

    // Unlike withdraw(), deposit() does not enforce account status locally:
    // blocking deposits on frozen or closed accounts is txn::validate's job.
    // This split is deliberate.
    void deposit(Money amount)
    {
        // No-op. Zero deposits are legal but produce no state change and
        // no version bump.
        if (amount.cents() == 0) return;
        balance_ = balance_ + amount;
        ++version_;
    }

    // A frozen or closed account never transacts, regardless of the amount.
    [[nodiscard]] bool can_withdraw(Money amount) const noexcept
    {
        return status_ == AccountStatus::Active && balance_.can_subtract(amount);
    }

    void withdraw(Money amount)
    {
        if (status_ != AccountStatus::Active) {
            throw std::domain_error{"Account: transaction refused on a frozen or closed account"};
        }
        if (!balance_.can_subtract(amount)) {
            throw std::domain_error{"Account: insufficient funds"};
        }
        balance_ = balance_ - amount;
        ++version_;
    }

    void set_status(AccountStatus status)
    {
        if (status_ == status) return;
        status_ = status;
        ++version_;
    }

private:
    AccountId id_;
    CustomerId owner_id_;
    Money balance_;
    AccountStatus status_;
    // TODO: version is unused until the concurrency milestone; will
    //       drive optimistic-retry on save().
    uint64_t version_;
};

} // namespace bank