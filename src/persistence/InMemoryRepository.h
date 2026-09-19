#pragma once

#include "persistence/Repository.h"

#include <map>
#include <vector>

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

class InMemoryCustomerRepository final : public CustomerRepository {
public:
    std::optional<Customer> find_by_id(CustomerId id) const override
    {
        const auto it = customers_.find(id.value());
        if (it == customers_.end()) return std::nullopt;
        return it->second;
    }

    void save(const Customer& customer) override
    {
        customers_.insert_or_assign(customer.id().value(), customer);
    }

private:
    std::map<uint64_t, Customer> customers_;
};

class InMemoryTransactionRepository final : public TransactionRepository {
public:
    void append(const TransactionRecord& record) override
    {
        records_.push_back(record);
    }

    std::vector<TransactionRecord> for_account(AccountId id) const override
    {
        std::vector<TransactionRecord> out;
        for (const auto& record : records_) {
            const bool involves =
                (record.from && *record.from == id) ||
                (record.to   && *record.to   == id);
            if (involves) out.push_back(record);
        }
        return out;
    }

    std::vector<TransactionRecord> all() const override
    {
        return records_;
    }

private:
    std::vector<TransactionRecord> records_;
};

class InMemoryAuditRepository final : public AuditRepository {
public:
    void append(const AuditEntry& entry) override
    {
        entries_.push_back(entry);
    }

    std::vector<AuditEntry> find(const AuditFilter& filter) const override
    {
        std::vector<AuditEntry> out;
        for (const auto& entry : entries_) {
            if (filter.action && *filter.action != entry.action) continue;
            if (filter.actor && *filter.actor != entry.actor_id) continue;
            if (filter.since && entry.timestamp < *filter.since) continue;
            out.push_back(entry);
        }
        return out;
    }

private:
    std::vector<AuditEntry> entries_;
};

} // namespace bank