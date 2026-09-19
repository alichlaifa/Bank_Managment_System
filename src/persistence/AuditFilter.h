#pragma once

#include "core/TimeUtil.h"
#include "domain/Types.h"

#include <optional>
#include <string>

namespace bank {

// Query shape for the audit trail. Omitted fields are not filtered on.
struct AuditFilter {
    std::optional<std::string> action;
    std::optional<CustomerId> actor;
    std::optional<timeutil::Clock::time_point> since;
};

} // namespace bank