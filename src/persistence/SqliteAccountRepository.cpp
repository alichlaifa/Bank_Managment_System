#include "persistence/SqliteAccountRepository.h"

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
using sqlite_detail::read_account;

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

} // namespace bank