#include <gtest/gtest.h>
#include "domain/Ostream.h"

#include "domain/Account.h"
#include "domain/Transaction.h"
#include "persistence/Serializer.h"

using namespace bank;

TEST(Codec, TransactionRecordRoundTripsATransfer)
{
    TransactionRecord record;
    record.id = TransactionId{11};
    record.timestamp = timeutil::from_unix_ms(1'800'000'000'000LL);
    record.type = TransactionType::Transfer;
    record.state = TransactionState::Committed;
    record.from = AccountId{2};
    record.to = AccountId{9};
    record.amount = Money{12'345};

    const auto line = codec::serialize(record);
    const auto decoded = codec::deserialize_transaction(line);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->id, record.id);
    ASSERT_TRUE(timeutil::to_unix_ms(decoded->timestamp) == 1'800'000'000'000LL);
    ASSERT_EQ(decoded->type, TransactionType::Transfer);
    ASSERT_EQ(decoded->state, TransactionState::Committed);
    ASSERT_TRUE(decoded->from == AccountId{2});
    ASSERT_TRUE(decoded->to == AccountId{9});
    ASSERT_EQ(decoded->amount, Money{12'345});
}

TEST(Codec, DepositNullSidesArePreserved)
{
    const TransactionRecord record = project_transaction(
        TransactionId{5}, Deposit{AccountId{1}, Money{100}});
    const auto decoded = codec::deserialize_transaction(codec::serialize(record));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_FALSE(decoded->from.has_value());
    ASSERT_TRUE(decoded->to == AccountId{1});
}

TEST(Codec, CorruptAndIncompleteLinesReturnNullopt)
{
    ASSERT_FALSE(codec::deserialize_transaction(""));
    ASSERT_FALSE(codec::deserialize_transaction("hello world"));
    ASSERT_FALSE(codec::deserialize_transaction(R"({"id":12})"));
    ASSERT_FALSE(codec::deserialize_transaction(R"({"id":12,"amount":-5})"));
}

TEST(Codec, AccountRecordsRoundTrip)
{
    const Account account{AccountId{3}, CustomerId{1}, Money{77'000},
                          AccountStatus::Frozen, 4};
    const auto line = codec::serialize(account);
    const auto decoded = codec::deserialize_account(line);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->id(), AccountId{3});
    ASSERT_EQ(decoded->owner_id(), CustomerId{1});
    ASSERT_EQ(decoded->balance(), Money{77'000});
    ASSERT_EQ(decoded->status(), AccountStatus::Frozen);
    ASSERT_TRUE(decoded->version() == 4);
}
