#pragma once

#include "persistence/CustomerRepository.h"

#include <map>

namespace bank {

// Container-backed repository implementations. Used by unit tests and as a
// dependency-free fallback. They are intentionally not thread-safe: the
// concurrency control lives in the transaction processor above them.
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

} // namespace bank