#include "TestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace bank;

namespace {

struct ThrowingFindAuditRepository : AuditRepository {
    void append(const AuditEntry&) override {}

    std::vector<AuditEntry> find(const AuditFilter&) const override
    {
        throw std::runtime_error{"audit store unavailable"};
    }
};

void seed_lifecycle_fixture(ServiceHarness& harness,
                            Money first = Money{1000},
                            Money second = Money{500})
{
    seed_customer(harness.customers, CustomerId{1}, "1234", Role::Customer);
    seed_customer(harness.customers, CustomerId{2}, "5678", Role::Teller);
    seed_account(harness.accounts, AccountId{1}, CustomerId{1}, first);
    seed_account(harness.accounts, AccountId{2}, CustomerId{2}, second);
}

std::size_t audit_count(const AuditRepository& audit, std::string_view action)
{
    AuditFilter filter;
    filter.action = std::string{action};
    return audit.find(filter).size();
}

bool involves(const TransactionRecord& record, AccountId id)
{
    return (record.from && *record.from == id) ||
           (record.to && *record.to == id);
}

} // namespace

TEST(AuditService, FindWithEmptyFilterReturnsAllEntries)
{
    InMemoryAuditRepository repo;
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.login",
                                     "success")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{2}, "auth.login_failed",
                                     "invalid credentials")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "account.frozen",
                                     "account 1")
                    .ok());
    AuditService service{repo};

    const auto result = service.find(AuditFilter{});

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().size(), std::size_t{3});
}

TEST(AuditService, FindByActorFilters)
{
    InMemoryAuditRepository repo;
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.login",
                                     "success")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{2}, "auth.login",
                                     "success")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.logout",
                                     "success")
                    .ok());
    AuditService service{repo};

    const auto result = service.find_by_actor(CustomerId{1});

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), std::size_t{2});
    for (const AuditEntry& entry : result.value()) {
        EXPECT_EQ(entry.actor_id, CustomerId{1});
    }
}

TEST(AuditService, FindByActionFilters)
{
    InMemoryAuditRepository repo;
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.login",
                                     "success")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.login_failed",
                                     "invalid credentials")
                    .ok());
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.logout",
                                     "success")
                    .ok());
    AuditService service{repo};

    const auto result = service.find_by_action("auth.login_failed");

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), std::size_t{1});
    EXPECT_EQ(result.value().front().action, "auth.login_failed");
}

TEST(AuditService, FindSinceFilters)
{
    const auto base = timeutil::now();
    InMemoryAuditRepository repo;

    AuditEntry old_entry;
    old_entry.id = 1;
    old_entry.timestamp = base - std::chrono::hours{1};
    old_entry.actor_id = CustomerId{1};
    old_entry.action = "old";
    repo.append(old_entry);

    AuditEntry middle_entry;
    middle_entry.id = 2;
    middle_entry.timestamp = base - std::chrono::minutes{30};
    middle_entry.actor_id = CustomerId{1};
    middle_entry.action = "middle";
    repo.append(middle_entry);

    AuditEntry new_entry;
    new_entry.id = 3;
    new_entry.timestamp = base;
    new_entry.actor_id = CustomerId{1};
    new_entry.action = "new";
    repo.append(new_entry);

    AuditService service{repo};
    const auto result =
        service.find_since(base - std::chrono::minutes{15});

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), std::size_t{1});
    EXPECT_EQ(result.value().front().action, "new");
}

TEST(AuditService, FindUnknownActorReturnsEmptySuccess)
{
    InMemoryAuditRepository repo;
    ASSERT_TRUE(audit_detail::append(repo, CustomerId{1}, "auth.login",
                                     "success")
                    .ok());
    AuditService service{repo};

    const auto result = service.find_by_actor(CustomerId{999});

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().empty());
}

TEST(AuditService, RepositoryThrowBecomesFailure)
{
    ThrowingFindAuditRepository repo;
    AuditService service{repo};

    const auto result = service.find(AuditFilter{});

    ASSERT_TRUE(result.failed());
    EXPECT_FALSE(result.error().empty());
}

TEST(InterestService, StubAccrualReturnsSuccess)
{
    ServiceHarness harness{2, Money{1000}};

    const auto result = harness.interest.accrue_monthly_interest();

    EXPECT_TRUE(result.ok());
}

TEST(InterestService, StubAccrualDoesNotChangeBalances)
{
    ServiceHarness harness{2, Money{1000}};
    const Money before_total = total_balance(harness.accounts);
    const std::size_t before_transactions =
        transaction_count(harness.transactions);

    ASSERT_TRUE(harness.interest.accrue_monthly_interest().ok());

    EXPECT_EQ(total_balance(harness.accounts), before_total);
    EXPECT_EQ(transaction_count(harness.transactions), before_transactions);
    for (const Account& account : harness.accounts.all()) {
        EXPECT_EQ(account.balance(), Money{1000});
    }
}

TEST(InterestService, StubAccrualDoesNotWriteAudit)
{
    ServiceHarness harness{2, Money{1000}};
    const std::size_t before = harness.audit.find(AuditFilter{}).size();

    ASSERT_TRUE(harness.interest.accrue_monthly_interest().ok());

    EXPECT_EQ(harness.audit.find(AuditFilter{}).size(), before);
}

TEST(Integration, FullLifecycleLoginDepositWithdrawTransferHistoryBalance)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);

    const auto first_login = harness.auth.login(CustomerId{1}, "1234");
    ASSERT_TRUE(first_login.ok());
    const auto second_login = harness.auth.login(CustomerId{2}, "5678");
    ASSERT_TRUE(second_login.ok());

    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());

    const auto first_balance = harness.banking.balance(AccountId{1});
    ASSERT_TRUE(first_balance.ok());
    EXPECT_EQ(first_balance.value(), Money{1025});

    const auto second_balance = harness.banking.balance(AccountId{2});
    ASSERT_TRUE(second_balance.ok());
    EXPECT_EQ(second_balance.value(), Money{525});

    const auto first_history = harness.banking.history(AccountId{1});
    ASSERT_TRUE(first_history.ok());
    EXPECT_EQ(first_history.value().size(), std::size_t{3});

    const auto second_history = harness.banking.history(AccountId{2});
    ASSERT_TRUE(second_history.ok());
    EXPECT_EQ(second_history.value().size(), std::size_t{1});
}

TEST(Integration, TotalMoneyIsConservedAcrossLifecycle)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);
    const Money initial_total = total_balance(harness.accounts);

    ASSERT_TRUE(harness.auth.login(CustomerId{1}, "1234").ok());
    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());

    EXPECT_EQ(total_balance(harness.accounts),
              initial_total + Money{100} - Money{50});
}

TEST(Integration, AuditTrailContainsLoginAndTransactionEvents)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);

    ASSERT_TRUE(harness.auth.login(CustomerId{1}, "1234").ok());
    ASSERT_TRUE(harness.auth.login(CustomerId{2}, "5678").ok());
    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());

    EXPECT_TRUE(has_audit(harness.audit, "auth.login"));
    EXPECT_EQ(audit_count(harness.audit, "auth.login"), std::size_t{2});
    EXPECT_EQ(audit_count(harness.audit, "txn.completed"), std::size_t{3});
}

TEST(Integration, HistoryMatchesLedger)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);

    ASSERT_TRUE(harness.banking.deposit(AccountId{1}, Money{100}).ok());
    ASSERT_TRUE(harness.banking.withdraw(AccountId{1}, Money{50}).ok());
    ASSERT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{25}).ok());

    const std::vector<TransactionRecord> ledger = harness.transactions.all();
    for (const AccountId id : {AccountId{1}, AccountId{2}}) {
        const auto history = harness.banking.history(id);
        ASSERT_TRUE(history.ok());

        const auto expected = std::count_if(
            ledger.begin(), ledger.end(),
            [id](const TransactionRecord& record) {
                return involves(record, id);
            });
        EXPECT_EQ(history.value().size(),
                  static_cast<std::size_t>(expected));

        for (const TransactionRecord& record : history.value()) {
            EXPECT_TRUE(involves(record, id));
            const auto found = std::find_if(
                ledger.begin(), ledger.end(),
                [&record](const TransactionRecord& candidate) {
                    return candidate.id == record.id;
                });
            EXPECT_NE(found, ledger.end());
        }
    }
}

TEST(Integration, FreezeUnfreezeInLifecycle)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);

    ASSERT_TRUE(harness.banking.freeze(AccountId{2}).ok());
    EXPECT_TRUE(
        harness.banking.transfer(AccountId{1}, AccountId{2}, Money{10})
            .failed());
    EXPECT_TRUE(harness.banking.deposit(AccountId{2}, Money{10}).failed());

    ASSERT_TRUE(harness.banking.unfreeze(AccountId{2}).ok());
    ASSERT_TRUE(harness.banking.deposit(AccountId{2}, Money{10}).ok());

    EXPECT_TRUE(has_audit(harness.audit, "account.frozen"));
    EXPECT_TRUE(has_audit(harness.audit, "account.unfrozen"));
}

TEST(Integration, LoginThenLoggedOutTokenIsInvalid)
{
    ServiceHarness harness;
    seed_lifecycle_fixture(harness);

    const auto login = harness.auth.login(CustomerId{1}, "1234");
    ASSERT_TRUE(login.ok());
    ASSERT_TRUE(harness.sessions.validate(login.value().token).has_value());

    ASSERT_TRUE(harness.auth.logout(login.value().token).ok());

    EXPECT_FALSE(
        harness.sessions.validate(login.value().token).has_value());
}

TEST(Integration, ConcurrentTransfersThroughServiceConserveMoney)
{
    constexpr int kThreads = 4;
    constexpr int kTransfersPerThread = 100;
    constexpr int64_t kAmountCents = 5;

    ServiceHarness harness;
    seed_lifecycle_fixture(harness, Money{100000}, Money{100000});

    std::barrier start{kThreads};
    std::atomic<bool> any_failure{false};
    std::vector<std::jthread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&harness, &start, &any_failure, t] {
            start.arrive_and_wait();
            for (int i = 0; i < kTransfersPerThread; ++i) {
                const bool forward = ((t + i) % 2) == 0;
                const AccountId from = forward ? AccountId{1} : AccountId{2};
                const AccountId to = forward ? AccountId{2} : AccountId{1};
                if (harness.banking
                        .transfer(from, to, Money{kAmountCents})
                        .failed()) {
                    any_failure.store(true);
                }
            }
        });
    }

    workers.clear();

    EXPECT_FALSE(any_failure.load());
    EXPECT_EQ(total_balance(harness.accounts), Money{200000});
    EXPECT_EQ(transaction_count(harness.transactions),
              static_cast<std::size_t>(kThreads * kTransfersPerThread));
    EXPECT_EQ(audit_count(harness.audit, "txn.completed"),
              static_cast<std::size_t>(kThreads * kTransfersPerThread));
    for (const Account& account : harness.accounts.all()) {
        EXPECT_GE(account.balance().cents(), int64_t{0});
    }
}

TEST(Integration, BalanceReadsAreConsistentWithFinalHistory)
{
    constexpr int kThreads = 4;
    constexpr int kTransfersPerThread = 50;

    ServiceHarness harness;
    seed_lifecycle_fixture(harness, Money{50000}, Money{50000});

    std::barrier start{kThreads};
    std::atomic<bool> any_failure{false};
    std::vector<std::jthread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&harness, &start, &any_failure, t] {
            start.arrive_and_wait();
            for (int i = 0; i < kTransfersPerThread; ++i) {
                const bool forward = ((t + i) % 2) == 0;
                const AccountId from = forward ? AccountId{1} : AccountId{2};
                const AccountId to = forward ? AccountId{2} : AccountId{1};
                if (harness.banking.transfer(from, to, Money{10}).failed()) {
                    any_failure.store(true);
                }
            }
        });
    }

    workers.clear();

    ASSERT_FALSE(any_failure.load());
    const std::vector<TransactionRecord> ledger = harness.transactions.all();
    for (const AccountId id : {AccountId{1}, AccountId{2}}) {
        const auto balance = harness.banking.balance(id);
        ASSERT_TRUE(balance.ok());
        const auto fresh = harness.accounts.find_by_id(id);
        ASSERT_TRUE(fresh.has_value());
        EXPECT_EQ(balance.value(), fresh->balance());

        const auto history = harness.banking.history(id);
        ASSERT_TRUE(history.ok());
        const auto committed = std::count_if(
            ledger.begin(), ledger.end(),
            [id](const TransactionRecord& record) {
                return involves(record, id) &&
                       record.state == TransactionState::Committed;
            });
        EXPECT_EQ(history.value().size(),
                  static_cast<std::size_t>(committed));
    }
    EXPECT_EQ(total_balance(harness.accounts), Money{100000});
}
