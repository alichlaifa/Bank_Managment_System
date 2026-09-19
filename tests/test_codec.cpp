#include <doctest.h>

#include "domain/Entities.h"
#include "domain/Transaction.h"
#include "persistence/Serializer.h"

using namespace bank;

TEST_CASE("transaction record round-trips a transfer")
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
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->id == record.id);
    REQUIRE(timeutil::to_unix_ms(decoded->timestamp) == 1'800'000'000'000LL);
    REQUIRE(decoded->type == TransactionType::Transfer);
    REQUIRE(decoded->state == TransactionState::Committed);
    REQUIRE(decoded->from == AccountId{2});
    REQUIRE(decoded->to == AccountId{9});
    REQUIRE(decoded->amount == Money{12'345});
}

TEST_CASE("deposit null sides are preserved")
{
    const TransactionRecord record = project_transaction(
        TransactionId{5}, Deposit{AccountId{1}, Money{100}});
    const auto decoded = codec::deserialize_transaction(codec::serialize(record));
    REQUIRE(decoded.has_value());
    REQUIRE_FALSE(decoded->from.has_value());
    REQUIRE(decoded->to == AccountId{1});
}

TEST_CASE("corrupt and incomplete lines return nullopt")
{
    REQUIRE_FALSE(codec::deserialize_transaction(""));
    REQUIRE_FALSE(codec::deserialize_transaction("hello world"));
    REQUIRE_FALSE(codec::deserialize_transaction(R"({"id":12})"));
    REQUIRE_FALSE(codec::deserialize_transaction(R"({"id":12,"amount":-5})"));
}

TEST_CASE("account records round-trip")
{
    const Account account{AccountId{3}, CustomerId{1}, Money{77'000},
                          AccountStatus::Frozen, 4};
    const auto line = codec::serialize(account);
    const auto decoded = codec::deserialize_account(line);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->id() == AccountId{3});
    REQUIRE(decoded->owner_id() == CustomerId{1});
    REQUIRE(decoded->balance() == Money{77'000});
    REQUIRE(decoded->status() == AccountStatus::Frozen);
    REQUIRE(decoded->version() == 4);
}