#pragma once

#include "persistence/AccountRepository.h"

namespace bank {

class SqliteDatabase;

// SQLite-backed repository implementations. Statements are prepared per call;
// caching them would add mutable state for negligible gain at this scale, and
// per-call prepare keeps the repositories trivially safe to share.
class SqliteAccountRepository final : public AccountRepository {
public:
    explicit SqliteAccountRepository(SqliteDatabase& db);

    std::optional<Account> find_by_id(AccountId id) const override;
    std::vector<Account> all() const override;
    void save(const Account& account) override;
    void remove(AccountId id) override;

private:
    SqliteDatabase& db_;
};

} // namespace bank