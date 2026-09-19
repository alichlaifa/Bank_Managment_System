#pragma once

#include "persistence/Repository.h"

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

class SqliteCustomerRepository final : public CustomerRepository {
public:
    explicit SqliteCustomerRepository(SqliteDatabase& db);

    std::optional<Customer> find_by_id(CustomerId id) const override;
    void save(const Customer& customer) override;

private:
    SqliteDatabase& db_;
};

class SqliteTransactionRepository final : public TransactionRepository {
public:
    explicit SqliteTransactionRepository(SqliteDatabase& db);

    void append(const TransactionRecord& record) override;
    std::vector<TransactionRecord> for_account(AccountId id) const override;
    std::vector<TransactionRecord> all() const override;

private:
    SqliteDatabase& db_;
};

class SqliteAuditRepository final : public AuditRepository {
public:
    explicit SqliteAuditRepository(SqliteDatabase& db);

    void append(const AuditEntry& entry) override;
    std::vector<AuditEntry> find(const AuditFilter& filter) const override;

private:
    SqliteDatabase& db_;
};

} // namespace bank