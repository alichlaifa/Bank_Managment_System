#pragma once

#include "persistence/AuditRepository.h"

namespace bank {

class SqliteDatabase;

// SQLite-backed repository implementations. Statements are prepared per call;
// caching them would add mutable state for negligible gain at this scale, and
// per-call prepare keeps the repositories trivially safe to share.
class SqliteAuditRepository final : public AuditRepository {
public:
    explicit SqliteAuditRepository(SqliteDatabase& db);

    void append(const AuditEntry& entry) override;
    std::vector<AuditEntry> find(const AuditFilter& filter) const override;

private:
    SqliteDatabase& db_;
};

} // namespace bank