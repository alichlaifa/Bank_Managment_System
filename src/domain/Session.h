#pragma once

#include "core/TimeUtil.h"
#include "domain/Types.h"

#include <string>

namespace bank {

struct Session {
    std::string token;
    CustomerId customer_id;
    Role role;
    SessionState state{SessionState::Anonymous};
    timeutil::Clock::time_point expires_at;
};

} // namespace bank