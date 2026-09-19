#pragma once

#include "domain/Account.h"
#include "domain/AuditEntry.h"
#include "domain/Customer.h"
#include "domain/Transaction.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bank::sqlite_detail {

// Runs a statement lifecycle check: OK/ROW/DONE are the valid outcomes, any
// error is surfaced as a descriptive exception.
void expect_ok(int rc, sqlite3* db, const char* what);

class Statement {
public:
    Statement(sqlite3* db, const char* sql, const char* what);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

void bind_i64(sqlite3_stmt* stmt, int index, int64_t value);
void bind_text(sqlite3_stmt* stmt, int index, std::string_view value);
void bind_integer_id(sqlite3_stmt* stmt, int index, const std::optional<AccountId>& id);

int64_t column_i64(sqlite3_stmt* stmt, int index);
std::string column_text(sqlite3_stmt* stmt, int index);

// The read_* row readers below use positional column indices that must stay
// in sync with the SELECT column order in the SQL strings further down. When
// editing a query, update its reader in the same change.
Account read_account(sqlite3_stmt* stmt);
Customer read_customer(sqlite3_stmt* stmt);
TransactionRecord read_transaction(sqlite3_stmt* stmt);
AuditEntry read_audit(sqlite3_stmt* stmt);

} // namespace bank::sqlite_detail