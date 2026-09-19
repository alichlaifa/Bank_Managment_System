#pragma once

#include "domain/Transaction.h"

#include <vector>

namespace bank {

class TransactionRepository {
public:
    virtual ~TransactionRepository() = default;

    virtual void append(const TransactionRecord& record) = 0;
    [[nodiscard]] virtual std::vector<TransactionRecord> for_account(AccountId id) const = 0;
    [[nodiscard]] virtual std::vector<TransactionRecord> all() const = 0;
};

} // namespace bank