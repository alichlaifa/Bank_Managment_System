#include "persistence/SqliteCustomerRepository.h"

#include "persistence/SqliteDatabase.h"
#include "persistence/SqliteHelpers.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>

namespace bank {

using sqlite_detail::Statement;
using sqlite_detail::bind_i64;
using sqlite_detail::bind_text;
using sqlite_detail::expect_ok;
using sqlite_detail::read_customer;

// --- Customer ---------------------------------------------------------------

SqliteCustomerRepository::SqliteCustomerRepository(SqliteDatabase& db) : db_{db} {}

std::optional<Customer> SqliteCustomerRepository::find_by_id(CustomerId id) const
{
    Statement stmt{db_.raw(),
        "SELECT id, name, pin_algorithm, pin_salt, pin_digest, role "
        "FROM customers WHERE id = ?1;",
        "customers.find"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(id.value()));

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    expect_ok(rc, db_.raw(), "customers.find");
    return read_customer(stmt.get());
}

void SqliteCustomerRepository::save(const Customer& customer)
{
    Statement stmt{db_.raw(),
        "INSERT INTO customers (id, name, pin_algorithm, pin_salt, pin_digest, role) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, "
        "pin_algorithm=excluded.pin_algorithm, pin_salt=excluded.pin_salt, "
        "pin_digest=excluded.pin_digest, role=excluded.role;",
        "customers.save"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(customer.id().value()));
    bind_text(stmt.get(), 2, customer.name());
    bind_text(stmt.get(), 3, customer.pin_hash().algorithm);
    bind_text(stmt.get(), 4, customer.pin_hash().salt);
    bind_text(stmt.get(), 5, customer.pin_hash().digest);
    bind_text(stmt.get(), 6, to_string(customer.role()));

    const int rc = sqlite3_step(stmt.get());
    expect_ok(rc, db_.raw(), "customers.save");
}

} // namespace bank