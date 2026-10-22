#include "services/InterestService.h"

namespace bank {

InterestService::InterestService(AccountRepository& accounts,
                                 AuditRepository& audit)
    : accounts_{accounts}, audit_{audit}
{
}

Result<void> InterestService::accrue_monthly_interest()
{
    // TODO(interest-scheduling): post deposits via
    // TransactionProcessor when this is implemented.
    // This milestone ships a compiling no-op stub.
    return Result<void>::success();
}

} // namespace bank
