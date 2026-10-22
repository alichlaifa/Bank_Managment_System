#include "services/AuthService.h"

#include "domain/PasswordHasher.h"
#include "services/AuditService.h"

#include <string>
#include <utility>

namespace bank {

AuthService::AuthService(CustomerRepository& customers,
                         SessionManager& sessions,
                         AuditRepository& audit)
    : customers_{customers}, sessions_{sessions}, audit_{audit}
{
}

Result<Session> AuthService::login(CustomerId customer_id, std::string_view pin)
{
    auto customer = customers_.find_by_id(customer_id);
    bool pin_ok = false;
    if (customer) {
        pin_ok = PasswordHasher::verify(pin, customer->pin_hash());
    } else {
        // Fixed dummy hash: same PBKDF2 work as the existing-customer path,
        // so login timing does not reveal existence.
        static const PasswordHash dummy =
            PasswordHasher::create("dummy-pin-do-not-use");
        (void)PasswordHasher::verify(pin, dummy);
        pin_ok = false;
    }

    if (!pin_ok) {
        (void)audit_detail::append(audit_, customer_id,
                                   "auth.login_failed",
                                   "invalid credentials");
        return Result<Session>::failure("invalid credentials");
    }

    Session session = sessions_.create(customer->id(), customer->role());
    (void)audit_detail::append(audit_, customer->id(),
                               "auth.login", "success");
    return Result<Session>::success(std::move(session));
}

Result<void> AuthService::logout(std::string_view token)
{
    auto session = sessions_.revoke(token);
    if (!session) {
        return Result<void>::failure("invalid session");
    }
    (void)audit_detail::append(audit_, session->customer_id,
                               "auth.logout", "success");
    return Result<void>::success();
}

} // namespace bank
