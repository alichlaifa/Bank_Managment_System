#include "domain/TransactionOps.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace bank {
namespace {

const Account* find_account(std::span<const Account> accounts, AccountId id) noexcept
{
    for (const Account& account : accounts) {
        if (account.id() == id) return &account;
    }
    return nullptr;
}

Account& require_account(std::span<Account> accounts, AccountId id)
{
    for (Account& account : accounts) {
        if (account.id() == id) return account;
    }
    throw std::out_of_range{"apply: involved account missing from span"};
}

} // namespace

namespace txn {

TransactionType type_of(const Transaction& txn) noexcept
{
    return std::visit([](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Deposit>)  return TransactionType::Deposit;
        if constexpr (std::is_same_v<T, Withdraw>) return TransactionType::Withdraw;
        return TransactionType::Transfer;
    }, txn);
}

Money amount_of(const Transaction& txn) noexcept
{
    return std::visit([](const auto& x) { return x.amount; }, txn);
}

std::optional<AccountId> debit_account(const Transaction& txn) noexcept
{
    if (const auto* w = std::get_if<Withdraw>(&txn)) return w->account_id;
    if (const auto* t = std::get_if<Transfer>(&txn)) return t->from;
    return std::nullopt;
}

std::optional<AccountId> credit_account(const Transaction& txn) noexcept
{
    if (const auto* d = std::get_if<Deposit>(&txn)) return d->account_id;
    if (const auto* t = std::get_if<Transfer>(&txn)) return t->to;
    return std::nullopt;
}

std::vector<AccountId> involved_accounts(const Transaction& txn)
{
    std::vector<AccountId> ids;
    const auto push_unique = [&ids](std::optional<AccountId> maybe) {
        if (maybe.has_value() &&
            std::find(ids.begin(), ids.end(), *maybe) == ids.end()) {
            ids.push_back(*maybe);
        }
    };
    push_unique(debit_account(txn));
    push_unique(credit_account(txn));
    return ids;
}

bool validate(const Transaction& txn, std::span<const Account> accounts)
{
    return std::visit([&accounts](const auto& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Deposit>) {
            const Account* target = find_account(accounts, x.account_id);
            return target != nullptr && target->status() == AccountStatus::Active
                && x.amount.is_positive();
        } else if constexpr (std::is_same_v<T, Withdraw>) {
            const Account* source = find_account(accounts, x.account_id);
            return source != nullptr && x.amount.is_positive() && source->can_withdraw(x.amount);
        } else {
            if (x.from == x.to || !x.amount.is_positive()) return false;
            const Account* debit  = find_account(accounts, x.from);
            const Account* credit = find_account(accounts, x.to);
            return debit != nullptr && credit != nullptr
                && debit->status() == AccountStatus::Active
                && credit->status() == AccountStatus::Active
                && debit->balance().can_subtract(x.amount);
        }
    }, txn);
}

void apply(const Transaction& txn, std::span<Account> accounts)
{
    if (!validate(txn, accounts)) {
        throw std::domain_error{"apply: transaction violates bank rules"};
    }

    std::visit([&accounts](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Deposit>) {
            require_account(accounts, x.account_id).deposit(x.amount);
        } else if constexpr (std::is_same_v<T, Withdraw>) {
            require_account(accounts, x.account_id).withdraw(x.amount);
        } else {
            // Debit first; if crediting the destination ever fails the caller's
            // compensation logic rolls the whole transaction back.
            require_account(accounts, x.from).withdraw(x.amount);
            require_account(accounts, x.to).deposit(x.amount);
        }
    }, txn);
}

} // namespace txn

} // namespace bank