#pragma once

#include "persistence/AccountRepository.h"

#include <map>

namespace bank {

// Container-backed repository implementations. Used by unit tests and as a
// dependency-free fallback. They are intentionally not thread-safe: the
// concurrency control lives in the transaction processor above them.
class InMemoryAccountRepository final : public AccountRepository {
public:
    std::optional<Account> find_by_id(AccountId id) const override
    {
        const auto it = accounts_.find(id.value());
        if (it == accounts_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<Account> all() const override
    {
        std::vector<Account> out;
        out.reserve(accounts_.size());
        for (const auto& [_, account] : accounts_) out.push_back(account);
        return out;
    }

    void save(const Account& account) override
    {
        accounts_.insert_or_assign(account.id().value(), account);
    }

    void remove(AccountId id) override
    {
        accounts_.erase(id.value());
    }

private:
    std::map<uint64_t, Account> accounts_;
};

} // namespace bank