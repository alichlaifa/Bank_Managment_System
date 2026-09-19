#include "persistence/SqliteAuditRepository.h"

#include "persistence/SqliteDatabase.h"
#include "persistence/SqliteHelpers.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bank {

using sqlite_detail::Statement;
using sqlite_detail::bind_i64;
using sqlite_detail::bind_text;
using sqlite_detail::expect_ok;
using sqlite_detail::read_audit;

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