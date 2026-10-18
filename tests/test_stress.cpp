#include <gtest/gtest.h>

#include "TestSupport.h"
#include "concurrency/TransactionProcessor.h"
#include "persistence/InMemoryAuditRepository.h"
#include "persistence/InMemoryTransactionRepository.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace bank;
using namespace std::chrono_literals;

namespace {

// Harness shared by the stress suites: N funded accounts + processor.
// Shared via shared_ptr so a watchdog-detached run cannot dangle.
struct StressHarness {
    StressHarness(std::size_t workers, std::size_t account_count, Money initial)
        : dir{"stress"},
          wal{dir.path / "wal.log"},
          processor{workers, accounts, transactions, audit, wal}
    {
        ids = seed_accounts(accounts, account_count, initial);
    }

    TempDir dir;
    InMemoryAccountRepository accounts;
    InMemoryTransactionRepository transactions;
    InMemoryAuditRepository audit;
    WalLogger wal;
    TransactionProcessor processor;
    std::vector<AccountId> ids;
};

int64_t total_balance(const InMemoryAccountRepository& repo)
{
    int64_t sum = 0;
    for (const Account& account : repo.all()) {
        sum += account.balance().cents();
    }
    return sum;
}

} // namespace

TEST(Stress, ConcurrentTransfersConserveMoney)
{
#ifdef __SANITIZE_THREAD__
    constexpr int kThreads = 8;
    constexpr int kTransfersPerThread = 100;  // smaller under TSan
#else
    constexpr int kThreads = 8;
    constexpr int kTransfersPerThread = 500;
#endif
    constexpr std::size_t kAccounts = 1000;
    constexpr int64_t kInitialCents = 100'000;  // $1000 per account

    // Shared so a watchdog-detached run cannot dangle.
    auto harness = std::make_shared<StressHarness>(0, kAccounts, Money{kInitialCents});
    const int64_t starting_total = total_balance(harness->accounts);
    ASSERT_EQ(starting_total, static_cast<int64_t>(kAccounts) * kInitialCents);

    std::atomic<int> succeeded{0};
    std::atomic<int> skipped{0};

    const bool finished = completes_within(
        [harness, &succeeded, &skipped] {
            std::vector<std::jthread> workers;
            workers.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                workers.emplace_back([harness, t, &succeeded, &skipped] {
                    std::mt19937 rng{static_cast<std::mt19937::result_type>(t + 1)};
                    std::uniform_int_distribution<std::uint64_t> account_dist{
                        0, harness->ids.size() - 1};
                    std::uniform_int_distribution<int64_t> amount_dist{1, 5'000};

                    for (int i = 0; i < kTransfersPerThread; ++i) {
                        const AccountId from = harness->ids[account_dist(rng)];
                        AccountId to = harness->ids[account_dist(rng)];
                        if (to == from) {
                            to = harness->ids[(account_dist(rng) + 1) % harness->ids.size()];
                        }
                        // Attempting is the race-safe balance check: the
                        // processor validates under the account locks, so a
                        // too-poor source fails cleanly and is skipped. (A
                        // pre-read here would race on the non-thread-safe
                        // repository.)
                        const Result<TransactionRecord> result = harness->processor.process_one(
                            Transfer{from, to, Money{amount_dist(rng)}});
                        if (result.ok()) {
                            succeeded.fetch_add(1);
                        } else {
                            skipped.fetch_add(1);
                            EXPECT_FALSE(result.error().empty());
                        }
                    }
                });
            }
            workers.clear();  // join
        },
        60s);

    ASSERT_TRUE(finished) << "stress run deadlocked or hung (watchdog tripped)";

    // Conservation: every committed transfer moves money, never creates or
    // destroys it; skipped transfers touch nothing.
    EXPECT_EQ(total_balance(harness->accounts), starting_total);

    const int attempts = kThreads * kTransfersPerThread;
    EXPECT_EQ(succeeded.load() + skipped.load(), attempts);
    EXPECT_GT(succeeded.load(), 0);  // the run must actually transfer something

    // WAL and repository agree with each other: one committed record each.
    EXPECT_EQ(harness->transactions.all().size(),
              harness->wal.load_all().size());
    for (const auto& record : harness->wal.load_all()) {
        EXPECT_EQ(record.state, TransactionState::Committed);
    }
}

TEST(Stress, DisjointAccountsWithoutLocksStillSafe)
{
    constexpr int kThreads = 4;
    constexpr std::size_t kAccountsPerThread = 50;
    constexpr int kOpsPerThread = 100;
    constexpr std::size_t kAccounts = kThreads * kAccountsPerThread;

    auto harness =
        std::make_shared<StressHarness>(0, kAccounts, Money{100'000});
    const int64_t starting_total = total_balance(harness->accounts);
    std::atomic<int> failures{0};

    const bool finished = completes_within(
        [harness, &failures] {
            std::vector<std::jthread> workers;
            workers.reserve(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                workers.emplace_back([harness, t, &failures] {
                    const auto begin =
                        harness->ids.begin() +
                        static_cast<std::ptrdiff_t>(t) * static_cast<std::ptrdiff_t>(kAccountsPerThread);
                    const auto end = begin + static_cast<std::ptrdiff_t>(kAccountsPerThread);

                    for (int i = 0; i < kOpsPerThread; ++i) {
                        Result<TransactionRecord> result =
                            harness->processor.process_one(Deposit{*begin, Money{100}});
                        if (!result.ok()) failures.fetch_add(1);

                        result = harness->processor.process_one(Withdraw{*begin, Money{100}});
                        if (!result.ok()) failures.fetch_add(1);

                        // Transfer strictly within this thread's slice.
                        auto from_it = begin;
                        auto to_it = begin + 1;
                        result = harness->processor.process_one(
                            Transfer{*from_it, *to_it, Money{50}});
                        if (!result.ok()) failures.fetch_add(1);
                        (void)end;
                    }
                });
            }
            workers.clear();
        },
        30s);

    ASSERT_TRUE(finished) << "disjoint-accounts run hung (watchdog tripped)";
    EXPECT_EQ(failures.load(), 0) << "every operation on a private slice must succeed";
    EXPECT_EQ(total_balance(harness->accounts), starting_total);
}

TEST(Stress, RepeatedStartStopCycles)
{
    constexpr int kCycles = 5;
    constexpr int kPerCycle = 10;

    auto harness = std::make_shared<StressHarness>(2, 4, Money{100'000});

    const bool finished = completes_within(
        [harness] {
            for (int cycle = 0; cycle < kCycles; ++cycle) {
                harness->processor.start();
                for (int i = 0; i < kPerCycle; ++i) {
                    const Result<TransactionId> id = harness->processor.submit(
                        Deposit{harness->ids[static_cast<std::size_t>(i % 4)], Money{10}});
                    EXPECT_TRUE(id.ok()) << id.error();
                }
                harness->processor.stop();

                // After stop(): queue drained, everything committed.
                EXPECT_EQ(harness->processor.pending(), 0u);
                const std::size_t expected =
                    static_cast<std::size_t>((cycle + 1) * kPerCycle);
                EXPECT_EQ(harness->transactions.all().size(), expected);
                EXPECT_EQ(harness->wal.load_all().size(), expected);
            }
        },
        30s);

    ASSERT_TRUE(finished) << "start/stop cycle run hung (watchdog tripped)";

    // Each account receives a deposit whenever i%4 selects it.
    for (std::size_t idx = 0; idx < harness->ids.size(); ++idx) {
        const auto account = harness->accounts.find_by_id(harness->ids[idx]);
        ASSERT_TRUE(account.has_value());
        int deposits = 0;
        for (int cycle = 0; cycle < kCycles; ++cycle) {
            for (int i = 0; i < kPerCycle; ++i) {
                if (static_cast<std::size_t>(i % 4) == idx) {
                    ++deposits;
                }
            }
        }
        EXPECT_EQ(account->balance().cents(), 100'000 + deposits * 10);
    }
}
