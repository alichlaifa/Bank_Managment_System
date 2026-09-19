#include "persistence/SqliteTransactionRepository.h"

#include "persistence/SqliteDatabase.h"
#include "persistence/SqliteHelpers.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>

namespace bank {

using sqlite_detail::Statement;
using sqlite_detail::bind_i64;
using sqlite_detail::bind_integer_id;
using sqlite_detail::bind_text;
using sqlite_detail::expect_ok;
using sqlite_detail::read_transaction;

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

} // namespace bank