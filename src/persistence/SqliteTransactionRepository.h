#pragma once

#include "persistence/TransactionRepository.h"

namespace bank {

class SqliteDatabase;

// SQLite-backed repository implementations. Statements are prepared per call;
// caching them would add mutable state for negligible gain at this scale, and
// per-call prepare keeps the repositories trivially safe to share.
class SqliteTransactionRepository final : public TransactionRepository {
public:
    explicit SqliteTransactionRepository(SqliteDatabase& db);

    void append(const TransactionRecord& record) override;
    std::vector<TransactionRecord> for_account(AccountId id) const override;
    std::vector<TransactionRecord> all() const override;

private:
    SqliteDatabase& db_;
};

} // namespace bank