#pragma once

#include "core/Result.h"
#include "persistence/AccountRepository.h"
#include "persistence/AuditRepository.h"

namespace bank {

class InterestService {
public:
    InterestService(AccountRepository& accounts,
                    AuditRepository& audit);

    [[nodiscard]] Result<void> accrue_monthly_interest();

private:
    AccountRepository& accounts_;
    AuditRepository& audit_;
};

} // namespace bank
