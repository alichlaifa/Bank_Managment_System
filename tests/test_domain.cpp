#include <gtest/gtest.h>
#include "domain/Ostream.h"

#include "domain/Account.h"
#include "domain/Transaction.h"
#include "domain/TransactionOps.h"

using namespace bank;

namespace {

Account make_account(AccountId id, Money balance, AccountStatus status = AccountStatus::Active)
{
    return Account{id, CustomerId{100}, balance, status, 0};
}

} // namespace

TEST(Domain, DepositCreditsBalanceAndBumpsVersion)
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.deposit(Money{500});
    ASSERT_EQ(account.balance(), Money{1'500});
    ASSERT_TRUE(account.version() == 1);
}

TEST(Domain, ZeroDepositIsANoOp)
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.deposit(Money{0});
    ASSERT_TRUE(account.version() == 0);
}

TEST(Domain, WithdrawEnforcesBalanceAndAccountStatus)
{
    Account account = make_account(AccountId{1}, Money{1'000});
    account.withdraw(Money{400});
    ASSERT_EQ(account.balance(), Money{600});
    ASSERT_TRUE(account.version() == 1);

    ASSERT_THROW(account.withdraw(Money{601}), std::domain_error);

    Account frozen = make_account(AccountId{2}, Money{5'000}, AccountStatus::Frozen);
    ASSERT_FALSE(frozen.can_withdraw(Money{1}));
    ASSERT_THROW(frozen.withdraw(Money{1}), std::domain_error);
}

TEST(Domain, SetStatusBumpsVersionButSkipsNoOpFreezes)
{
    Account account = make_account(AccountId{1}, Money{100});
    account.set_status(AccountStatus::Frozen);
    ASSERT_EQ(account.status(), AccountStatus::Frozen);
    ASSERT_TRUE(account.version() == 1);
    account.set_status(AccountStatus::Frozen);
    ASSERT_TRUE(account.version() == 1);
}

TEST(Domain, DepositsOnAFrozenAccountAreRefusedByTheTransactionLayer)
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{1'000}, AccountStatus::Frozen)};
    ASSERT_FALSE(txn::validate(Deposit{AccountId{1}, Money{250}}, accounts));
}

TEST(Domain, ValidateChecksAmountPositivityAndAccountExistence)
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{1'000})};
    ASSERT_TRUE(txn::validate(Deposit{AccountId{1}, Money{250}}, accounts));
    ASSERT_FALSE(txn::validate(Deposit{AccountId{1}, Money{0}}, accounts));
    ASSERT_FALSE(txn::validate(Deposit{AccountId{42}, Money{250}}, accounts));
}

TEST(Domain, WithdrawValidation)
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{100})};
    ASSERT_TRUE(txn::validate(Withdraw{AccountId{1}, Money{100}}, accounts));
    ASSERT_FALSE(txn::validate(Withdraw{AccountId{1}, Money{101}}, accounts));
}

TEST(Domain, TransferRejectsSelfCreditAndOverdrafts)
{
    std::vector<Account> accounts{
        make_account(AccountId{1}, Money{100}),
        make_account(AccountId{2}, Money{50})
    };
    ASSERT_FALSE(txn::validate(Transfer{AccountId{1}, AccountId{1}, Money{10}}, accounts));
    ASSERT_FALSE(txn::validate(Transfer{AccountId{1}, AccountId{2}, Money{101}}, accounts));
    ASSERT_TRUE(txn::validate(Transfer{AccountId{1}, AccountId{2}, Money{50}}, accounts));
}

TEST(Domain, ApplyMutatesBothLegsOfATransferAtomically)
{
    std::vector<Account> accounts{
        make_account(AccountId{1}, Money{100}),
        make_account(AccountId{2}, Money{50})
    };
    txn::apply(Transfer{AccountId{1}, AccountId{2}, Money{30}}, accounts);
    ASSERT_EQ(accounts[0].balance(), Money{70});
    ASSERT_EQ(accounts[1].balance(), Money{80});
}

TEST(Domain, ApplyRefusesAnInvalidTransaction)
{
    std::vector<Account> accounts{make_account(AccountId{1}, Money{10})};
    ASSERT_THROW(txn::apply(Withdraw{AccountId{1}, Money{999}}, accounts), std::domain_error);
}

TEST(Domain, ProjectTransactionCarriesTheCorrectDirectionFields)
{
    const TransactionRecord deposit = project_transaction(
        TransactionId{7}, Deposit{AccountId{3}, Money{200}});
    ASSERT_EQ(deposit.type, TransactionType::Deposit);
    ASSERT_FALSE(deposit.from.has_value());
    ASSERT_TRUE(deposit.to == AccountId{3});
    ASSERT_EQ(deposit.state, TransactionState::Pending);

    const TransactionRecord transfer = project_transaction(
        TransactionId{8}, Transfer{AccountId{4}, AccountId{5}, Money{100}});
    ASSERT_EQ(transfer.from, AccountId{4});
    ASSERT_EQ(transfer.to, AccountId{5});
}

TEST(Domain, MaterializeRebuildsTheVariantFromARecord)
{
    const auto record = project_transaction(
        TransactionId{1}, Deposit{AccountId{9}, Money{333}});
    const auto txn = materialize_transaction(record);
    ASSERT_EQ(txn::type_of(txn), TransactionType::Deposit);
    ASSERT_EQ(txn::amount_of(txn), Money{333});
}

TEST(Domain, DescribeProducesAReadableSummary)
{
    const std::string text = describe(Deposit{AccountId{1}, Money{10'000}});
    ASSERT_TRUE(text.find("deposit") != std::string::npos);
    ASSERT_TRUE(text.find("$100.00") != std::string::npos);
}

TEST(Domain, InvolvedAccountsDeduplicatesTheCreditDebitSide)
{
    auto ids = txn::involved_accounts(Deposit{AccountId{1}, Money{100}});
    ASSERT_TRUE(ids.size() == 1);
    ASSERT_EQ(ids[0], AccountId{1});

    ids = txn::involved_accounts(Transfer{AccountId{2}, AccountId{3}, Money{50}});
    ASSERT_TRUE(ids.size() == 2);
}
