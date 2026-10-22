#include "TestSupport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace bank;

TEST(BankingService, DepositReturnsCommittedRecord)
{
    ServiceHarness harness{1, Money{0}};

    const auto result = harness.banking.deposit(AccountId{1}, Money{500});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().state, TransactionState::Committed);
    EXPECT_EQ(result.value().type, TransactionType::Deposit);
    EXPECT_EQ(result.value().amount, Money{500});
}

TEST(BankingService, DepositUpdatesBalanceAndHistory)
{
    ServiceHarness harness{1, Money{100}};

    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{50}).ok());

    const auto balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(balance.ok());
    EXPECT_EQ(balance.value(), Money{150});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{1});
}

TEST(BankingService, DepositMissingAccountFails)
{
    ServiceHarness harness{1, Money{100}};

    const auto result = harness.banking.deposit(AccountId{2}, Money{50});

    ASSERT_TRUE(result.failed());
    EXPECT_NE(result.error().find("account not found"), std::string::npos);
    const auto account = harness.accounts.find_by_id(AccountId{1});
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->balance(), Money{100});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, WithdrawReturnsCommittedRecord)
{
    ServiceHarness harness{1, Money{500}};

    const auto result = harness.banking.withdraw(AccountId{1}, Money{200});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().state, TransactionState::Committed);
    EXPECT_EQ(result.value().type, TransactionType::Withdraw);
    const auto balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(balance.ok());
    EXPECT_EQ(balance.value(), Money{300});
}

TEST(BankingService, WithdrawInsufficientFundsFails)
{
    ServiceHarness harness{1, Money{100}};

    const auto result = harness.banking.withdraw(AccountId{1}, Money{200});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    const auto balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(balance.ok());
    EXPECT_EQ(balance.value(), Money{100});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, WithdrawMissingAccountFails)
{
    ServiceHarness harness{1, Money{100}};

    const auto result = harness.banking.withdraw(AccountId{2}, Money{10});

    ASSERT_TRUE(result.failed());
    EXPECT_NE(result.error().find("account not found"), std::string::npos);
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, TransferMovesFundsAtomically)
{
    ServiceHarness harness{2, Money{1000}};

    const auto result =
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{300});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().type, TransactionType::Transfer);
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{700});
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{2})->balance(),
              Money{1300});
    EXPECT_EQ(total_balance(harness.accounts), Money{2000});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{1});
}

TEST(BankingService, TransferInsufficientFundsFails)
{
    ServiceHarness harness{2, Money{100}};

    const auto result =
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{500});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{100});
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{2})->balance(),
              Money{100});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, TransferMissingSourceFails)
{
    ServiceHarness harness{1, Money{500}};

    const auto result =
        harness.banking.transfer(AccountId{99}, AccountId{1}, Money{10});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, TransferMissingDestinationFails)
{
    ServiceHarness harness{1, Money{500}};

    const auto result =
        harness.banking.transfer(AccountId{1}, AccountId{99}, Money{10});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, TransferToSameAccountFails)
{
    ServiceHarness harness{1, Money{500}};

    const auto result =
        harness.banking.transfer(AccountId{1}, AccountId{1}, Money{10});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, BalanceReturnsCurrentMoney)
{
    ServiceHarness harness{1, Money{100}};

    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{300}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{100}).ok());

    const auto balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(balance.ok());
    EXPECT_EQ(balance.value(), Money{300});
    const auto account = harness.accounts.find_by_id(AccountId{1});
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(balance.value(), account->balance());
}

TEST(BankingService, BalanceMissingAccountFails)
{
    ServiceHarness harness{1, Money{100}};

    const auto result = harness.banking.balance(AccountId{2});

    ASSERT_TRUE(result.failed());
    EXPECT_NE(result.error().find("account not found"), std::string::npos);
}

TEST(BankingService, HistoryReturnsAllRecordsForAccount)
{
    ServiceHarness harness{2, Money{1000}};
    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());

    const auto first = harness.banking.history(AccountId{1});
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().size(), std::size_t{3});

    const auto second = harness.banking.history(AccountId{2});
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().size(), std::size_t{1});
    EXPECT_EQ(second.value().front().type, TransactionType::Transfer);
}

TEST(BankingService, HistoryUnknownAccountReturnsEmptySuccess)
{
    ServiceHarness harness{1, Money{100}};

    const auto result = harness.banking.history(AccountId{99});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(BankingService, FreezeRejectsDeposit)
{
    ServiceHarness harness{1, Money{500}};
    ASSERT_TRUE(harness.banking.freeze(AccountId{1}).ok());

    const auto result = harness.banking.deposit(AccountId{1}, Money{100});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
}

TEST(BankingService, FreezeRejectsWithdraw)
{
    ServiceHarness harness{1, Money{500}};
    ASSERT_TRUE(harness.banking.freeze(AccountId{1}).ok());

    const auto result = harness.banking.withdraw(AccountId{1}, Money{100});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
}

TEST(BankingService, FreezeRejectsTransfer)
{
    ServiceHarness harness{2, Money{500}};
    ASSERT_TRUE(harness.banking.freeze(AccountId{1}).ok());

    const auto result =
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{100});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{1})->balance(),
              Money{500});
    EXPECT_EQ(harness.accounts.find_by_id(AccountId{2})->balance(),
              Money{500});
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, FreezeMissingAccountFails)
{
    ServiceHarness harness{};

    const auto result = harness.banking.freeze(AccountId{1});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
}

TEST(BankingService, UnfreezeRestoresTransactions)
{
    ServiceHarness harness{1, Money{100}};
    ASSERT_TRUE(harness.banking.freeze(AccountId{1}).ok());
    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{50}).failed());

    ASSERT_TRUE(harness.banking.unfreeze(AccountId{1}).ok());
    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{50}).ok());

    const auto balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(balance.ok());
    EXPECT_EQ(balance.value(), Money{150});
}

TEST(BankingService, UnfreezeMissingAccountFails)
{
    ServiceHarness harness{};

    const auto result = harness.banking.unfreeze(AccountId{1});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
}

TEST(BankingService, FreezeAndUnfreezeWriteAuditEntries)
{
    ServiceHarness harness{1, Money{100}};

    ASSERT_TRUE(harness.banking.freeze(AccountId{1}).ok());
    ASSERT_TRUE(harness.banking.unfreeze(AccountId{1}).ok());

    EXPECT_TRUE(has_audit(harness.audit, "account.frozen"));
    EXPECT_TRUE(has_audit(harness.audit, "account.unfrozen"));
}

TEST(BankingService, SuccessfulMutationWritesExactlyOneTransactionRecord)
{
    ServiceHarness harness{2, Money{1000}};

    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{1});
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{2});
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());
    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{3});
}

TEST(BankingService, FailedMutationWritesNoTransactionRecord)
{
    ServiceHarness harness{2, Money{100}};

    EXPECT_TRUE(harness.banking.deposit(AccountId{99}, Money{10}).failed());
    EXPECT_TRUE(harness.banking.withdraw(AccountId{1}, Money{1000}).failed());
    EXPECT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{99}, Money{10})
            .failed());
    EXPECT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{1}, Money{10})
            .failed());

    EXPECT_EQ(transaction_count(harness.transactions), std::size_t{0});
}

TEST(BankingService, EveryFailureHasNonEmptyError)
{
    ServiceHarness harness{2, Money{100}};

    const auto missing = harness.banking.deposit(AccountId{99}, Money{10});
    ASSERT_TRUE(missing.failed());
    EXPECT_FALSE(missing.error().empty());

    const auto insufficient =
        harness.banking.withdraw(AccountId{1}, Money{1000});
    ASSERT_TRUE(insufficient.failed());
    EXPECT_FALSE(insufficient.error().empty());

    ASSERT_TRUE(harness.banking.freeze(AccountId{2}).ok());
    const auto frozen = harness.banking.deposit(AccountId{2}, Money{10});
    ASSERT_TRUE(frozen.failed());
    EXPECT_FALSE(frozen.error().empty());
}

TEST(BankingService, ConcurrentDepositsConserveMoney)
{
    constexpr int kThreads = 4;
    constexpr int kDepositsPerThread = 100;
    constexpr int64_t kDepositCents = 10;

    ServiceHarness harness{
        static_cast<std::size_t>(kThreads), Money{0}};

    std::barrier start{kThreads};
    std::atomic<bool> any_failure{false};
    std::vector<std::jthread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&harness, &start, &any_failure, t] {
            const AccountId account{static_cast<uint64_t>(t + 1)};
            start.arrive_and_wait();
            for (int i = 0; i < kDepositsPerThread; ++i) {
                if (harness.banking.deposit(account, Money{kDepositCents})
                        .failed()) {
                    any_failure.store(true);
                }
            }
        });
    }

    workers.clear();

    EXPECT_FALSE(any_failure.load());
    const int64_t expected =
        static_cast<int64_t>(kThreads) *
        static_cast<int64_t>(kDepositsPerThread) * kDepositCents;
    EXPECT_EQ(total_balance(harness.accounts), Money{expected});
    EXPECT_EQ(transaction_count(harness.transactions),
              static_cast<std::size_t>(kThreads * kDepositsPerThread));
}
