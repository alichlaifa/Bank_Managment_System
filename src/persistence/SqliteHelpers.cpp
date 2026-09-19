#include "persistence/SqliteHelpers.h"

#include <stdexcept>
#include <string>

namespace bank::sqlite_detail {

void expect_ok(int rc, sqlite3* db, const char* what)
{
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) return;
    throw std::runtime_error{std::string{what} + " failed: " + sqlite3_errmsg(db)};
}

Statement::Statement(sqlite3* db, const char* sql, const char* what)
{
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error{std::string{what} + " prepare failed: " + sqlite3_errmsg(db)};
    }
}

Statement::~Statement()
{
    sqlite3_finalize(stmt_);
}

void bind_i64(sqlite3_stmt* stmt, int index, int64_t value)
{
    const int rc = sqlite3_bind_int64(stmt, index, value);
    expect_ok(rc, sqlite3_db_handle(stmt), "bind int64");
}

void bind_text(sqlite3_stmt* stmt, int index, std::string_view value)
{
    const int rc = sqlite3_bind_text(stmt, index, value.data(),
                                     static_cast<int>(value.size()), SQLITE_TRANSIENT);
    expect_ok(rc, sqlite3_db_handle(stmt), "bind text");
}

void bind_integer_id(sqlite3_stmt* stmt, int index, const std::optional<AccountId>& id)
{
    if (id.has_value()) {
        bind_i64(stmt, index, static_cast<int64_t>(id->value()));
    } else {
        const int rc = sqlite3_bind_null(stmt, index);
        expect_ok(rc, sqlite3_db_handle(stmt), "bind null");
    }
}

int64_t column_i64(sqlite3_stmt* stmt, int index)
{
    return sqlite3_column_int64(stmt, index);
}

std::string column_text(sqlite3_stmt* stmt, int index)
{
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return {};
    const auto* text = sqlite3_column_text(stmt, index);
    return std::string{reinterpret_cast<const char*>(text)};
}

// The read_* row readers below use positional column indices that must stay
// in sync with the SELECT column order in the SQL strings further down. When
// editing a query, update its reader in the same change.
Account read_account(sqlite3_stmt* stmt)
{
    const AccountId id{static_cast<uint64_t>(column_i64(stmt, 0))};
    const CustomerId owner{static_cast<uint64_t>(column_i64(stmt, 1))};
    const Money balance{column_i64(stmt, 2)};
    const auto status = account_status_from_string(column_text(stmt, 3));
    if (!status) throw std::runtime_error{"corrupt account row: unknown status"};
    const uint64_t version = static_cast<uint64_t>(column_i64(stmt, 4));
    return Account{id, owner, balance, *status, version};
}

Customer read_customer(sqlite3_stmt* stmt)
{
    const CustomerId id{static_cast<uint64_t>(column_i64(stmt, 0))};
    PasswordHash pin_hash;
    pin_hash.algorithm = column_text(stmt, 2);
    pin_hash.salt      = column_text(stmt, 3);
    pin_hash.digest    = column_text(stmt, 4);
    const auto role = role_from_string(column_text(stmt, 5));
    if (!role) throw std::runtime_error{"corrupt customer row: unknown role"};
    return Customer{id, column_text(stmt, 1), std::move(pin_hash), *role};
}

TransactionRecord read_transaction(sqlite3_stmt* stmt)
{
    TransactionRecord record;
    record.id = TransactionId{static_cast<uint64_t>(column_i64(stmt, 0))};
    record.timestamp = timeutil::from_unix_ms(column_i64(stmt, 1));
    const auto type  = transaction_type_from_string(column_text(stmt, 2));
    const auto state = transaction_state_from_string(column_text(stmt, 3));
    if (!type || !state) throw std::runtime_error{"corrupt transaction row"};

    record.type = *type;
    record.state = *state;
    if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
        record.from = std::nullopt;
    } else {
        record.from = AccountId{static_cast<uint64_t>(column_i64(stmt, 4))};
    }
    if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
        record.to = std::nullopt;
    } else {
        record.to = AccountId{static_cast<uint64_t>(column_i64(stmt, 5))};
    }
    record.amount = Money{column_i64(stmt, 6)};
    return record;
}

AuditEntry read_audit(sqlite3_stmt* stmt)
{
    AuditEntry entry;
    entry.id = static_cast<uint64_t>(column_i64(stmt, 0));
    entry.timestamp = timeutil::from_unix_ms(column_i64(stmt, 1));
    entry.actor_id = CustomerId{static_cast<uint64_t>(column_i64(stmt, 2))};
    entry.action = column_text(stmt, 3);
    entry.details = column_text(stmt, 4);
    return entry;
}

} // namespace bank::sqlite_detail