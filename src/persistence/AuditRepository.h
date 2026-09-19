#pragma once

#include "domain/AuditEntry.h"
#include "persistence/AuditFilter.h"

#include <vector>

namespace bank {

class AuditRepository {
public:
    virtual ~AuditRepository() = default;

    virtual void append(const AuditEntry& entry) = 0;
    [[nodiscard]] virtual std::vector<AuditEntry> find(const AuditFilter& filter) const = 0;
};

} // namespace bank