#include <doctest.h>

#include "domain/Money.h"

#include <limits>

using bank::Money;

TEST_CASE("Money stores integer cents exactly")
{
    Money amount{Money::zero()};
    REQUIRE(amount.cents() == 0);
    REQUIRE(Money{123'456}.cents() == 123'456);
}

TEST_CASE("Money rejects negative construction")
{
    REQUIRE_THROWS_AS(Money{-1}, std::invalid_argument);
}

TEST_CASE("Money subtraction cannot go below zero")
{
    const Money balance{Money{1'000}};
    REQUIRE((balance - Money{100}).cents() == 900);
    REQUIRE_THROWS_AS(balance - Money{1'001}, std::underflow_error);
}

TEST_CASE("Money addition overflow is detected")
{
    const Money huge{std::numeric_limits<int64_t>::max()};
    REQUIRE_THROWS_AS(huge + Money{1}, std::overflow_error);
}

TEST_CASE("Money comparisons are total")
{
    REQUIRE(Money{10} == Money{10});
    REQUIRE(Money{5} < Money{10});
    REQUIRE(Money{99} > Money{1});
}

TEST_CASE("can_subtract answers without throwing")
{
    const Money balance{Money{500}};
    REQUIRE(balance.can_subtract(Money{500}));
    REQUIRE_FALSE(balance.can_subtract(Money{501}));
}