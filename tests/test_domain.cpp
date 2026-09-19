#include <doctest.h>

#include "domain/Entities.h"
#include "domain/Transaction.h"

using namespace bank;

namespace {

Account make_account(AccountId id, Money balance, AccountStatus status = AccountStatus::Active)
{
    return Account{id, CustomerId{100}, balance, status, 0};
}

} // namespace

TEST_CASE("deposit credits balance and bumps version")
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.deposit(Money{500});
    REQUIRE(account.balance() == Money{1'500});
    REQUIRE(account.version() == 1);
}

TEST_CASE("zero deposit is a no-op")
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.deposit(Money{0});
    REQUIRE(account.version() == 0);
}

TEST_CASE("withdraw enforces balance and account status")
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.withdraw(Money{400});
    REQUIRE(account.balance() == Money{600});
    REQUIRE(account.version() == 1);

    REQUIRE_THROWS_AS(account.withdraw(Money{601}), std::domain_error);

    Account frozen = make_account(AccountId{2}, Money{5'000}, AccountStatus::Frozen);
    REQUIRE_FALSE(frozen.can_withdraw(Money{1}));
    REQUIRE_THROWS_AS(frozen.withdraw(Money{1}), std::domain_error);
}

TEST_CASE("set_status bumps version but skips no-op freezes")
{
    Account account = make_account(AccountId{1}, Money{100});
    account.set_status(AccountStatus::Frozen);
    REQUIRE(account.status() == AccountStatus::Frozen);
    REQUIRE(account.version() == 1);
    account.set_status(AccountStatus::Frozen);
    REQUIRE(account.version() == 1);
}

TEST_CASE("deposits on a frozen account are refused by the transaction layer")
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{1'000}, AccountStatus::Frozen)};
    REQUIRE_FALSE(txn::validate(Deposit{AccountId{1}, Money{250}}, accounts));
}

TEST_CASE("validate checks amount positivity and account existence")
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{1'000})};
    REQUIRE(txn::validate(Deposit{AccountId{1}, Money{250}}, accounts));
    REQUIRE_FALSE(txn::validate(Deposit{AccountId{1}, Money{0}}, accounts));
    REQUIRE_FALSE(txn::validate(Deposit{AccountId{42}, Money{250}}, accounts));
}

TEST_CASE("withdraw validation")
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{100})};
    REQUIRE(txn::validate(Withdraw{AccountId{1}, Money{100}}, accounts));
    REQUIRE_FALSE(txn::validate(Withdraw{AccountId{1}, Money{101}}, accounts));
}

TEST_CASE("transfer rejects self-credit and overdrafts")
{
    std::vector<Account> accounts{
        make_account(AccountId{1}, Money{100}),
        make_account(AccountId{2}, Money{50})
    };
    REQUIRE_FALSE(txn::validate(Transfer{AccountId{1}, AccountId{1}, Money{10}}, accounts));
    REQUIRE_FALSE(txn::validate(Transfer{AccountId{1}, AccountId{2}, Money{101}}, accounts));
    REQUIRE(txn::validate(Transfer{AccountId{1}, AccountId{2}, Money{50}}, accounts));
}

TEST_CASE("apply mutates both legs of a transfer atomically")
{
    std::vector<Account> accounts{
        make_account(AccountId{1}, Money{100}),
        make_account(AccountId{2}, Money{50})
    };
    txn::apply(Transfer{AccountId{1}, AccountId{2}, Money{30}}, accounts);
    REQUIRE(accounts[0].balance() == Money{70});
    REQUIRE(accounts[1].balance() == Money{80});
}

TEST_CASE("apply refuses an invalid transaction")
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{10})};
    REQUIRE_THROWS_AS(txn::apply(Withdraw{AccountId{1}, Money{999}}, accounts), std::domain_error);
}

TEST_CASE("project_transaction carries the correct direction fields")
{
    const TransactionRecord deposit = project_transaction(
        TransactionId{7}, Deposit{AccountId{3}, Money{200}});
    REQUIRE(deposit.type == TransactionType::Deposit);
    REQUIRE_FALSE(deposit.from.has_value());
    REQUIRE(deposit.to == AccountId{3});
    REQUIRE(deposit.state == TransactionState::Pending);

    const TransactionRecord transfer = project_transaction(
        TransactionId{8}, Transfer{AccountId{4}, AccountId{5}, Money{100}});
    REQUIRE(transfer.from == AccountId{4});
    REQUIRE(transfer.to == AccountId{5});
}

TEST_CASE("materialize rebuilds the variant from a record")
{
    const auto record = project_transaction(
        TransactionId{1}, Deposit{AccountId{9}, Money{333}});
    const auto txn = materialize_transaction(record);
    REQUIRE(txn::type_of(txn) == TransactionType::Deposit);
    REQUIRE(txn::amount_of(txn) == Money{333});
}

TEST_CASE("describe produces a readable summary")
{
    const std::string text = describe(Deposit{AccountId{1}, Money{10'000}});
    REQUIRE(text.find("deposit") != std::string::npos);
    REQUIRE(text.find("$100.00") != std::string::npos);
}

TEST_CASE("involved_accounts deduplicates the credit/debit side")
{
    auto ids = txn::involved_accounts(Deposit{AccountId{1}, Money{100}});
    REQUIRE(ids.size() == 1);
    REQUIRE(ids[0] == AccountId{1});

    ids = txn::involved_accounts(Transfer{AccountId{2}, AccountId{3}, Money{50}});
    REQUIRE(ids.size() == 2);
}