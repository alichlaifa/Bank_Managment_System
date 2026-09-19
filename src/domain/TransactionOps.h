#pragma once

#include "domain/Transaction.h"
#include "domain/Account.h"

#include <optional>
#include <span>
#include <vector>

namespace bank::txn {

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

} // namespace bank::txn