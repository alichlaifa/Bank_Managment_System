#pragma once

#include "domain/Types.h"
#include "domain/PasswordHash.h"
#include "domain/PasswordHasher.h"

#include <string>
#include <string_view>

namespace bank {

class Customer {
public:
    Customer(CustomerId id, std::string name, PasswordHash pin_hash, Role role)
        : id_{id}, name_{std::move(name)}, pin_hash_{std::move(pin_hash)}, role_{role}
    {
    }

    [[nodiscard]] CustomerId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const PasswordHash& pin_hash() const noexcept { return pin_hash_; }
    [[nodiscard]] Role role() const noexcept { return role_; }

    [[nodiscard]] bool verify_pin(std::string_view pin) const
    {
        return PasswordHasher::verify(pin, pin_hash_);
    }

private:
    CustomerId id_;
    std::string name_;
    PasswordHash pin_hash_;
    Role role_;
};

} // namespace bank