#pragma once

#include "persistence/TransactionRepository.h"

#include <vector>

namespace bank {

// Container-backed repository implementations. Used by unit tests and as a
// dependency-free fallback. They are intentionally not thread-safe: the
// concurrency control lives in the transaction processor above them.
class InMemoryTransactionRepository final : public TransactionRepository {
public:
    void append(const TransactionRecord& record) override
    {
        records_.push_back(record);
    }

    std::vector<TransactionRecord> for_account(AccountId id) const override
    {
        std::vector<TransactionRecord> out;
        for (const auto& record : records_) {
            const bool involves =
                (record.from && *record.from == id) ||
                (record.to   && *record.to   == id);
            if (involves) out.push_back(record);
        }
        return out;
    }

    std::vector<TransactionRecord> all() const override
    {
        return records_;
    }

private:
    std::vector<TransactionRecord> records_;
};

} // namespace bank