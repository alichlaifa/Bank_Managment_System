#pragma once

#include "core/Result.h"
#include "core/TimeUtil.h"
#include "domain/AuditEntry.h"
#include "domain/Types.h"
#include "persistence/AuditFilter.h"
#include "persistence/AuditRepository.h"

#include <string>
#include <vector>

namespace bank {

namespace audit_detail {

// Serializes all service-originated audit writes and allocates IDs from one
// process-wide counter. Defined in AuditService.cpp.
[[nodiscard]] Result<void> append(AuditRepository& audit,
                                  CustomerId actor,
                                  std::string action,
                                  std::string details);

} // namespace audit_detail

class AuditService {
public:
    explicit AuditService(AuditRepository& audit);

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find(const AuditFilter& filter) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_by_actor(CustomerId actor) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_by_action(std::string action) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_since(timeutil::Clock::time_point since) const;

private:
    AuditRepository& audit_;
};

} // namespace bank
