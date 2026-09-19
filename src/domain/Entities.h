#pragma once

#include "core/TimeUtil.h"
#include "domain/Money.h"
#include "domain/Password.h"
#include "domain/Types.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace bank {

class Customer {
public:
    Customer(CustomerId id, std::string name, PasswordHash pin_hash, Role role)
        : id_{id}, name_{std::move(name)}, pin_hash_{std::move(pin_hash)}, role_{role}
    {
    }

    [[nodiscard]] CustomerId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const PasswordHash& pin_hash() const noexcept { return pin_hash_; }
    [[nodiscard]] Role role() const noexcept { return role_; }

    [[nodiscard]] bool verify_pin(std::string_view pin) const
    {
        return PasswordHasher::verify(pin, pin_hash_);
    }

private:
    CustomerId id_;
    std::string name_;
    PasswordHash pin_hash_;
    Role role_;
};

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

struct Session {
    std::string token;
    CustomerId customer_id;
    Role role;
    SessionState state{SessionState::Anonymous};
    timeutil::Clock::time_point expires_at;
};

// Append-only audit record. Ids are assigned by the caller (the audit service);
// the timestamp is wall-clock time at write time.
struct AuditEntry {
    uint64_t id{0};
    timeutil::Clock::time_point timestamp;
    CustomerId actor_id;
    std::string action;
    std::string details;
};

} // namespace bank