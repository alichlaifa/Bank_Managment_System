#include "persistence/SqliteRepository.h"

#include "persistence/SqliteDatabase.h"

#include <sqlite3.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bank {
namespace {

// Runs a statement lifecycle check: OK/ROW/DONE are the valid outcomes, any
// error is surfaced as a descriptive exception.
void expect_ok(int rc, sqlite3* db, const char* what)
{
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) return;
    throw std::runtime_error{std::string{what} + " failed: " + sqlite3_errmsg(db)};
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql, const char* what)
    {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error{std::string{what} + " prepare failed: " + sqlite3_errmsg(db)};
        }
    }

    ~Statement()
    {
        sqlite3_finalize(stmt_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

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

} // namespace

// --- Account ---------------------------------------------------------------

SqliteAccountRepository::SqliteAccountRepository(SqliteDatabase& db) : db_{db} {}

std::optional<Account> SqliteAccountRepository::find_by_id(AccountId id) const
{
    Statement stmt{db_.raw(),
        "SELECT id, owner_id, balance_cents, status, version FROM accounts WHERE id = ?1;",
        "accounts.find"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(id.value()));

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    expect_ok(rc, db_.raw(), "accounts.find");
    return read_account(stmt.get());
}

std::vector<Account> SqliteAccountRepository::all() const
{
    Statement stmt{db_.raw(),
        "SELECT id, owner_id, balance_cents, status, version FROM accounts ORDER BY id;",
        "accounts.all"};
    std::vector<Account> out;
    for (int rc = sqlite3_step(stmt.get()); rc == SQLITE_ROW;
         rc = sqlite3_step(stmt.get())) {
        out.push_back(read_account(stmt.get()));
    }
    return out;
}

void SqliteAccountRepository::save(const Account& account)
{
    Statement stmt{db_.raw(),
        "INSERT INTO accounts (id, owner_id, balance_cents, status, version) "
        "VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(id) DO UPDATE SET owner_id=excluded.owner_id, "
        "balance_cents=excluded.balance_cents, status=excluded.status, "
        "version=excluded.version;",
        "accounts.save"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(account.id().value()));
    bind_i64(stmt.get(), 2, static_cast<int64_t>(account.owner_id().value()));
    bind_i64(stmt.get(), 3, account.balance().cents());
    bind_text(stmt.get(), 4, to_string(account.status()));
    bind_i64(stmt.get(), 5, static_cast<int64_t>(account.version()));

    const int rc = sqlite3_step(stmt.get());
    expect_ok(rc, db_.raw(), "accounts.save");
}

void SqliteAccountRepository::remove(AccountId id)
{
    Statement stmt{db_.raw(), "DELETE FROM accounts WHERE id = ?1;", "accounts.remove"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(id.value()));
    const int rc = sqlite3_step(stmt.get());
    expect_ok(rc, db_.raw(), "accounts.remove");
}

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

// --- Transaction -------------------------------------------------------------

SqliteTransactionRepository::SqliteTransactionRepository(SqliteDatabase& db) : db_{db} {}

void SqliteTransactionRepository::append(const TransactionRecord& record)
{
    Statement stmt{db_.raw(),
        "INSERT INTO transactions (id, timestamp_ms, type, state, from_id, to_id, amount_cents) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
        "transactions.append"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(record.id.value()));
    bind_i64(stmt.get(), 2, timeutil::to_unix_ms(record.timestamp));
    bind_text(stmt.get(), 3, to_string(record.type));
    bind_text(stmt.get(), 4, to_string(record.state));
    bind_integer_id(stmt.get(), 5, record.from);
    bind_integer_id(stmt.get(), 6, record.to);
    bind_i64(stmt.get(), 7, record.amount.cents());

    const int rc = sqlite3_step(stmt.get());
    expect_ok(rc, db_.raw(), "transactions.append");
}

std::vector<TransactionRecord> SqliteTransactionRepository::for_account(AccountId id) const
{
    Statement stmt{db_.raw(),
        "SELECT id, timestamp_ms, type, state, from_id, to_id, amount_cents "
        "FROM transactions WHERE from_id = ?1 OR to_id = ?1 ORDER BY id;",
        "transactions.for_account"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(id.value()));

    std::vector<TransactionRecord> out;
    for (int rc = sqlite3_step(stmt.get()); rc == SQLITE_ROW;
         rc = sqlite3_step(stmt.get())) {
        out.push_back(read_transaction(stmt.get()));
    }
    return out;
}

std::vector<TransactionRecord> SqliteTransactionRepository::all() const
{
    Statement stmt{db_.raw(),
        "SELECT id, timestamp_ms, type, state, from_id, to_id, amount_cents "
        "FROM transactions ORDER BY id;",
        "transactions.all"};
    std::vector<TransactionRecord> out;
    for (int rc = sqlite3_step(stmt.get()); rc == SQLITE_ROW;
         rc = sqlite3_step(stmt.get())) {
        out.push_back(read_transaction(stmt.get()));
    }
    return out;
}

// --- Audit -------------------------------------------------------------------

SqliteAuditRepository::SqliteAuditRepository(SqliteDatabase& db) : db_{db} {}

void SqliteAuditRepository::append(const AuditEntry& entry)
{
    Statement stmt{db_.raw(),
        "INSERT INTO audit_log (id, timestamp_ms, actor_id, action, details) "
        "VALUES (?1, ?2, ?3, ?4, ?5);",
        "audit.append"};
    bind_i64(stmt.get(), 1, static_cast<int64_t>(entry.id));
    bind_i64(stmt.get(), 2, timeutil::to_unix_ms(entry.timestamp));
    bind_i64(stmt.get(), 3, static_cast<int64_t>(entry.actor_id.value()));
    bind_text(stmt.get(), 4, entry.action);
    bind_text(stmt.get(), 5, entry.details);

    const int rc = sqlite3_step(stmt.get());
    expect_ok(rc, db_.raw(), "audit.append");
}

std::vector<AuditEntry> SqliteAuditRepository::find(const AuditFilter& filter) const
{
    std::string sql = "SELECT id, timestamp_ms, actor_id, action, details FROM audit_log";

    // Filter ordering is fixed: action, actor, since. Parameters are bound in
    // the same order as the clauses below, so ?1/?2/?3 stay deterministic.
    std::vector<std::string> clauses;
    if (filter.action.has_value()) clauses.push_back("action = ?1");
    if (filter.actor.has_value())  clauses.push_back("actor_id = ?2");
    if (filter.since.has_value())  clauses.push_back("timestamp_ms >= ?3");

    if (!clauses.empty()) {
        sql += " WHERE ";
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            if (i != 0) sql += " AND ";
            sql += clauses[i];
        }
    }
    sql += " ORDER BY timestamp_ms;";

    Statement stmt{db_.raw(), sql.c_str(), "audit.find"};
    if (filter.action.has_value()) bind_text(stmt.get(), 1, *filter.action);
    if (filter.actor.has_value())  bind_i64(stmt.get(), 2, static_cast<int64_t>(filter.actor->value()));
    if (filter.since.has_value())  bind_i64(stmt.get(), 3, timeutil::to_unix_ms(*filter.since));

    std::vector<AuditEntry> out;
    for (int rc = sqlite3_step(stmt.get()); rc == SQLITE_ROW;
         rc = sqlite3_step(stmt.get())) {
        out.push_back(read_audit(stmt.get()));
    }
    return out;
}

} // namespace bank