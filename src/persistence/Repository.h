#pragma once

#include "core/TimeUtil.h"
#include "domain/Entities.h"
#include "domain/Transaction.h"

#include <optional>
#include <string>
#include <vector>

namespace bank {

// Query shape for the audit trail. Omitted fields are not filtered on.
struct AuditFilter {
    std::optional<std::string> action;
    std::optional<CustomerId> actor;
    std::optional<timeutil::Clock::time_point> since;
};

// TODO: id assignment policy. Currently the caller assigns ids; consider
//       whether the repository or a dedicated sequencer should own this.
//
// Repository interfaces. Implementations (in-memory, SQLite) swap in under the
// service layer without the services knowing which engine is live.
class AccountRepository {
public:
    virtual ~AccountRepository() = default;

    [[nodiscard]] virtual std::optional<Account> find_by_id(AccountId id) const = 0;
    [[nodiscard]] virtual std::vector<Account> all() const = 0;
    virtual void save(const Account& account) = 0;
    virtual void remove(AccountId id) = 0;
};

class CustomerRepository {
public:
    virtual ~CustomerRepository() = default;

    [[nodiscard]] virtual std::optional<Customer> find_by_id(CustomerId id) const = 0;
    virtual void save(const Customer& customer) = 0;
};

class TransactionRepository {
public:
    virtual ~TransactionRepository() = default;

    virtual void append(const TransactionRecord& record) = 0;
    [[nodiscard]] virtual std::vector<TransactionRecord> for_account(AccountId id) const = 0;
    [[nodiscard]] virtual std::vector<TransactionRecord> all() const = 0;
};

class AuditRepository {
public:
    virtual ~AuditRepository() = default;

    virtual void append(const AuditEntry& entry) = 0;
    [[nodiscard]] virtual std::vector<AuditEntry> find(const AuditFilter& filter) const = 0;
};

} // namespace bank