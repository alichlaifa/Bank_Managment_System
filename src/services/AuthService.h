#pragma once

#include "core/Result.h"
#include "domain/Customer.h"
#include "domain/Session.h"
#include "domain/Types.h"
#include "persistence/AuditRepository.h"
#include "persistence/CustomerRepository.h"
#include "services/SessionManager.h"

#include <string>
#include <string_view>

namespace bank {

class AuthService {
public:
    AuthService(CustomerRepository& customers,
                SessionManager& sessions,
                AuditRepository& audit);

    [[nodiscard]] Result<Session> login(CustomerId customer_id,
                                        std::string_view pin);

    [[nodiscard]] Result<void> logout(std::string_view token);

private:
    [[nodiscard]] Result<void> audit(CustomerId actor,
                                     std::string action,
                                     std::string details);

    CustomerRepository& customers_;
    SessionManager& sessions_;
    AuditRepository& audit_;
};

} // namespace bank
