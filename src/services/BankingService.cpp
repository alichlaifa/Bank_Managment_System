#include "services/BankingService.h"

#include "domain/TransactionOps.h"
#include "services/AuditService.h"

#include <exception>
#include <string>
#include <utility>

namespace bank {

BankingService::BankingService(TransactionProcessor& processor,
                               AccountRepository& accounts,
                               TransactionRepository& transactions,
                               AuditRepository& audit)
    : processor_{processor},
      accounts_{accounts},
      transactions_{transactions},
      audit_{audit}
{
}

Result<TransactionRecord> BankingService::deposit(AccountId account_id,
                                                  Money amount)
{
    std::lock_guard lk{op_mutex_};
    return processor_.process_one(Deposit{account_id, amount});
}

Result<TransactionRecord> BankingService::withdraw(AccountId account_id,
                                                   Money amount)
{
    std::lock_guard lk{op_mutex_};
    return processor_.process_one(Withdraw{account_id, amount});
}

Result<TransactionRecord> BankingService::transfer(AccountId from,
                                                   AccountId to,
                                                   Money amount)
{
    std::lock_guard lk{op_mutex_};
    return processor_.process_one(Transfer{from, to, amount});
}

Result<Money> BankingService::balance(AccountId account_id) const
{
    std::lock_guard lk{op_mutex_};
    auto account = accounts_.find_by_id(account_id);
    if (!account) {
        return Result<Money>::failure(
            "account not found: " + std::to_string(account_id.value()));
    }
    return Result<Money>::success(account->balance());
}

Result<std::vector<TransactionRecord>>
BankingService::history(AccountId account_id) const
{
    std::lock_guard lk{op_mutex_};
    try {
        auto records = transactions_.for_account(account_id);
        return Result<std::vector<TransactionRecord>>::success(
            std::move(records));
    } catch (const std::exception& ex) {
        return Result<std::vector<TransactionRecord>>::failure(
            std::string{"history query failed: "} + ex.what());
    }
}

Result<void> BankingService::freeze(AccountId account_id)
{
    std::lock_guard lk{op_mutex_};
    auto account = accounts_.find_by_id(account_id);
    if (!account) {
        return Result<void>::failure("account not found");
    }
    if (account->status() == AccountStatus::Closed) {
        return Result<void>::failure("cannot freeze closed account");
    }
    if (account->status() == AccountStatus::Frozen) {
        return Result<void>::success();
    }
    Account updated = *account;
    updated.set_status(AccountStatus::Frozen);
    try {
        accounts_.save(updated);
    } catch (const std::exception& ex) {
        return Result<void>::failure(
            std::string{"freeze failed: "} + ex.what());
    }
    (void)audit_detail::append(audit_, CustomerId{0}, "account.frozen",
                               "account " + std::to_string(account_id.value()));
    return Result<void>::success();
}

Result<void> BankingService::unfreeze(AccountId account_id)
{
    std::lock_guard lk{op_mutex_};
    auto account = accounts_.find_by_id(account_id);
    if (!account) {
        return Result<void>::failure("account not found");
    }
    if (account->status() == AccountStatus::Closed) {
        return Result<void>::failure("cannot unfreeze closed account");
    }
    if (account->status() == AccountStatus::Active) {
        return Result<void>::success();
    }
    Account updated = *account;
    updated.set_status(AccountStatus::Active);
    try {
        accounts_.save(updated);
    } catch (const std::exception& ex) {
        return Result<void>::failure(
            std::string{"unfreeze failed: "} + ex.what());
    }
    (void)audit_detail::append(audit_, CustomerId{0}, "account.unfrozen",
                               "account " + std::to_string(account_id.value()));
    return Result<void>::success();
}

} // namespace bank
