#pragma once

#include "domain/Money.h"
#include "domain/Types.h"

#include <cstdint>
#include <ostream>

namespace bank {

// Stream overloads for the value types that appear in assertions. They give
// test failures a readable form ("$123.45" instead of an opaque byte dump).
inline std::ostream& operator<<(std::ostream& os, Money money)
{
    const int64_t cents = money.cents();
    const int64_t remainder = cents % 100;
    os << '$' << (cents / 100) << '.';
    if (remainder < 10) {
        os << '0';
    }
    return os << remainder;
}

template <typename Tag>
inline std::ostream& operator<<(std::ostream& os, Id<Tag> id)
{
    return os << id.value();
}

inline std::ostream& operator<<(std::ostream& os, Role role)
{
    return os << to_string(role);
}

inline std::ostream& operator<<(std::ostream& os, AccountStatus status)
{
    return os << to_string(status);
}

inline std::ostream& operator<<(std::ostream& os, TransactionType type)
{
    return os << to_string(type);
}

inline std::ostream& operator<<(std::ostream& os, TransactionState state)
{
    return os << to_string(state);
}

inline std::ostream& operator<<(std::ostream& os, SessionState state)
{
    return os << to_string(state);
}

} // namespace bank
