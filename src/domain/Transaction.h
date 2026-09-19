#pragma once

#include "core/TimeUtil.h"
#include "domain/Account.h"
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

} // namespace bank