#pragma once

#include "core/TimeUtil.h"
#include "domain/Entities.h"
#include "domain/Types.h"

#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bank {

// A transaction is one of three mutations. Using std::variant makes invalid
// states unrepresentable: a transfer always carries both sides, and money
// fields never coexist across types.
struct Deposit  { AccountId account_id; Money amount; };
struct Withdraw { AccountId account_id; Money amount; };
struct Transfer { AccountId from; AccountId to; Money amount; };

using Transaction = std::variant<Deposit, Withdraw, Transfer>;

// Flat, persistence-friendly projection of a transaction. This is the shape
// stored in SQLite and written to the WAL.
struct TransactionRecord {
    TransactionId id;
    timeutil::Clock::time_point timestamp;
    TransactionType type;
    TransactionState state;
    std::optional<AccountId> from;
    std::optional<AccountId> to;
    Money amount;
};

// Capture a transaction into a record stamped with the current wall clock.
TransactionRecord project_transaction(TransactionId id, const Transaction& txn);

// Inverse of project_transaction; used to re-apply persisted work on recovery.
Transaction materialize_transaction(const TransactionRecord& record);

// One-line human form, handy for audit trailers and diagnostics.
std::string describe(const Transaction& txn);

namespace txn {

TransactionType type_of(const Transaction& txn) noexcept;
Money amount_of(const Transaction& txn) noexcept;

// Side that gives money away / the side that receives it.
std::optional<AccountId> debit_account(const Transaction& txn) noexcept;
std::optional<AccountId> credit_account(const Transaction& txn) noexcept;

std::vector<AccountId> involved_accounts(const Transaction& txn);

// Checks the transaction against current account snapshots. Callers must pass
// every account named by involved_accounts() (extra accounts are harmless).
[[nodiscard]] bool validate(const Transaction& txn, std::span<const Account> accounts);

// Applies the transaction by mutating the passed accounts. Enforces the same
// rules as validate() and throws std::domain_error on violation, so an
// unvalidated apply cannot corrupt balances.
void apply(const Transaction& txn, std::span<Account> accounts);

} // namespace txn

} // namespace bank