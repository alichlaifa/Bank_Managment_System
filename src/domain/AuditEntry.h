#pragma once

#include "core/TimeUtil.h"
#include "domain/Types.h"

#include <cstdint>
#include <string>

namespace bank {

// Append-only audit record. Ids are assigned by the caller (the audit service);
// the timestamp is wall-clock time at write time.
struct AuditEntry {
    uint64_t id{0};
    timeutil::Clock::time_point timestamp;
    CustomerId actor_id;
    std::string action;
    std::string details;
};

} // namespace bank