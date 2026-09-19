#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace bank {

// All money is tracked as an integer count of cents. Floating point never
// appears on the financial path, so balances and transaction amounts are exact
// and comparisons are trivial.
class Money {
public:
    constexpr Money() noexcept = default;

    constexpr explicit Money(int64_t cents) : cents_{cents}
    {
        if (cents_ < 0) {
            throw std::invalid_argument{"Money: negative amounts are not representable"};
        }
    }

    [[nodiscard]] constexpr static Money zero() noexcept { return Money{0}; }

    [[nodiscard]] constexpr int64_t cents() const noexcept { return cents_; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return cents_ > 0; }

    [[nodiscard]] constexpr Money plus(Money other) const
    {
        if (other.cents_ > std::numeric_limits<int64_t>::max() - cents_) {
            throw std::overflow_error{"Money: addition overflow"};
        }
        return Money{cents_ + other.cents_};
    }

    [[nodiscard]] constexpr Money minus(Money other) const
    {
        if (other.cents_ > cents_) {
            throw std::underflow_error{"Money: subtraction would go below zero"};
        }
        return Money{cents_ - other.cents_};
    }

    [[nodiscard]] constexpr bool can_subtract(Money other) const noexcept
    {
        return other.cents_ <= cents_;
    }

    friend constexpr Money operator+(Money lhs, Money rhs) { return lhs.plus(rhs); }
    friend constexpr Money operator-(Money lhs, Money rhs) { return lhs.minus(rhs); }

    friend constexpr bool operator==(const Money&, const Money&) = default;
    friend constexpr auto operator<=>(const Money&, const Money&) = default;

private:
    int64_t cents_{0};
};

} // namespace bank