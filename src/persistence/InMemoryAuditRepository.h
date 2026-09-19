#pragma once

#include "persistence/AuditRepository.h"

#include <vector>

namespace bank {

// Container-backed repository implementations. Used by unit tests and as a
// dependency-free fallback. They are intentionally not thread-safe: the
// concurrency control lives in the transaction processor above them.
class InMemoryAuditRepository final : public AuditRepository {
public:
    void append(const AuditEntry& entry) override
    {
        entries_.push_back(entry);
    }

    std::vector<AuditEntry> find(const AuditFilter& filter) const override
    {
        std::vector<AuditEntry> out;
        for (const auto& entry : entries_) {
            if (filter.action && *filter.action != entry.action) continue;
            if (filter.actor && *filter.actor != entry.actor_id) continue;
            if (filter.since && entry.timestamp < *filter.since) continue;
            out.push_back(entry);
        }
        return out;
    }

private:
    std::vector<AuditEntry> entries_;
};

} // namespace bank