#pragma once

#include "domain/Customer.h"

#include <optional>

namespace bank {

class CustomerRepository {
public:
    virtual ~CustomerRepository() = default;

    [[nodiscard]] virtual std::optional<Customer> find_by_id(CustomerId id) const = 0;
    virtual void save(const Customer& customer) = 0;
};

} // namespace bank