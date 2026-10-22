#pragma once

#include "concurrency/TransactionProcessor.h"
#include "core/Result.h"
#include "domain/Account.h"
#include "domain/Money.h"
#include "domain/Transaction.h"
#include "domain/Types.h"
#include "persistence/AccountRepository.h"
#include "persistence/AuditRepository.h"
#include "persistence/TransactionRepository.h"

#include <mutex>
#include <vector>

namespace bank {

class BankingService {
public:
    BankingService(TransactionProcessor& processor,
                   AccountRepository& accounts,
                   TransactionRepository& transactions,
                   AuditRepository& audit);

    [[nodiscard]] Result<TransactionRecord> deposit(AccountId account_id,
                                                    Money amount);

    [[nodiscard]] Result<TransactionRecord> withdraw(AccountId account_id,
                                                     Money amount);

    [[nodiscard]] Result<TransactionRecord> transfer(AccountId from,
                                                     AccountId to,
                                                     Money amount);

    [[nodiscard]] Result<Money> balance(AccountId account_id) const;

    [[nodiscard]] Result<std::vector<TransactionRecord>>
    history(AccountId account_id) const;

    [[nodiscard]] Result<void> freeze(AccountId account_id);

    [[nodiscard]] Result<void> unfreeze(AccountId account_id);

private:
    TransactionProcessor& processor_;
    AccountRepository& accounts_;
    TransactionRepository& transactions_;
    AuditRepository& audit_;

    // Serializes every public BankingService call, including reads and
    // process_one mutations. Correctness over throughput: this milestone
    // cannot share the processor's private AccountLockManager, so a
    // service-owned mutex is the only way to keep balance()/history() from
    // racing service-issued writes. Direct processor.submit() callers bypass
    // this guarantee.
    mutable std::mutex op_mutex_;
};

} // namespace bank
