#pragma once

#include "persistence/CustomerRepository.h"

namespace bank {

class SqliteDatabase;

// SQLite-backed repository implementations. Statements are prepared per call;
// caching them would add mutable state for negligible gain at this scale, and
// per-call prepare keeps the repositories trivially safe to share.
class SqliteCustomerRepository final : public CustomerRepository {
public:
    explicit SqliteCustomerRepository(SqliteDatabase& db);

    std::optional<Customer> find_by_id(CustomerId id) const override;
    void save(const Customer& customer) override;

private:
    SqliteDatabase& db_;
};

} // namespace bank