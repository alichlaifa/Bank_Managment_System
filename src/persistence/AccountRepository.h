#pragma once

#include "domain/Account.h"

#include <optional>
#include <vector>

namespace bank {

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

} // namespace bank