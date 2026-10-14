#include <gtest/gtest.h>
#include "domain/Ostream.h"

#include "domain/Money.h"

#include <limits>

using bank::Money;

TEST(Money, StoresIntegerCentsExactly)
{
    Money amount{Money::zero()};
    ASSERT_TRUE(amount.cents() == 0);
    ASSERT_TRUE(Money{123'456}.cents() == 123'456);
}

TEST(Money, RejectsNegativeConstruction)
{
    ASSERT_THROW(Money{-1}, std::invalid_argument);
}

TEST(Money, SubtractionCannotGoBelowZero)
{
    const Money balance{Money{1'000}};
    ASSERT_TRUE((balance - Money{100}).cents() == 900);
    ASSERT_THROW(balance - Money{1'001}, std::underflow_error);
}

TEST(Money, AdditionOverflowIsDetected)
{
    const Money huge{std::numeric_limits<int64_t>::max()};
    ASSERT_THROW(huge + Money{1}, std::overflow_error);
}

TEST(Money, ComparisonsAreTotal)
{
    ASSERT_EQ(Money{10}, Money{10});
    ASSERT_TRUE(Money{5} < Money{10});
    ASSERT_TRUE(Money{99} > Money{1});
}

TEST(Money, CanSubtractAnswersWithoutThrowing)
{
    const Money balance{Money{500}};
    ASSERT_TRUE(balance.can_subtract(Money{500}));
    ASSERT_FALSE(balance.can_subtract(Money{501}));
}
