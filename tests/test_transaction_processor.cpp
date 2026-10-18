#include <gtest/gtest.h>

#include "TestSupport.h"
#include "concurrency/TransactionProcessor.h"
#include "persistence/InMemoryAuditRepository.h"
#include "persistence/InMemoryTransactionRepository.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace bank;
using namespace std::chrono_literals;

namespace {

// Single-threaded harness: 3 accounts with $1000 each, synchronous
// processor (worker_count = 0, start() never called) so every test is
// deterministic. WalLogger lives in a temp dir.
struct SyncHarness {
    explicit SyncHarness(Money initial = Money{100'000})
        : dir{"proc"},
          wal{dir.path / "wal.log"},
          processor{0, accounts, transactions, audit, wal}
    {
        ids = seed_accounts(accounts, 3, initial);
    }

    TempDir dir;
    InMemoryAccountRepository accounts;
    InMemoryTransactionRepository transactions;
    InMemoryAuditRepository audit;
    WalLogger wal;
    TransactionProcessor processor;
    std::vector<AccountId> ids;
};

int64_t balance_of(const InMemoryAccountRepository& repo, AccountId id)
{
    const auto account = repo.find_by_id(id);
    if (!account.has_value()) return -1;
    return account->balance().cents();
}

} // namespace

TEST(TransactionProcessor, DepositIncreasesBalanceAndCommits)
{
    SyncHarness h;

    const Result<TransactionRecord> result =
        h.processor.process_one(Deposit{h.ids[0], Money{500}});

    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_EQ(result.value().state, TransactionState::Committed);
    EXPECT_EQ(result.value().type, TransactionType::Deposit);
    EXPECT_EQ(balance_of(h.accounts, h.ids[0]), 100'000 + 500);

    const auto wal_records = h.wal.load_all();
    ASSERT_EQ(wal_records.size(), 1u);
    EXPECT_EQ(wal_records[0].state, TransactionState::Committed);
    EXPECT_EQ(wal_records[0].id, result.value().id);

    const auto txn_records = h.transactions.all();
    ASSERT_EQ(txn_records.size(), 1u);
    EXPECT_EQ(txn_records[0].id, result.value().id);
}

TEST(TransactionProcessor, WithdrawInsufficientFundsFails)
{
    SyncHarness h;

    const Result<TransactionRecord> result =
        h.processor.process_one(Withdraw{h.ids[0], Money{999'999}});

    ASSERT_TRUE(result.failed());
    ASSERT_FALSE(result.error().empty());  // Result<void>("") trap: never empty
    EXPECT_EQ(balance_of(h.accounts, h.ids[0]), 100'000);  // untouched
    EXPECT_TRUE(h.transactions.all().empty());

    // No committed WAL record for the failed withdrawal.
    for (const auto& record : h.wal.load_all()) {
        EXPECT_NE(record.state, TransactionState::Committed);
    }

    // The failure is in the audit trail.
    AuditFilter filter;
    filter.action = "txn.failed";
    EXPECT_FALSE(h.audit.find(filter).empty());
}

TEST(TransactionProcessor, TransferMovesFundsAtomically)
{
    SyncHarness h;
    const int64_t before_sum =
        balance_of(h.accounts, h.ids[0]) + balance_of(h.accounts, h.ids[1]);

    const Result<TransactionRecord> result =
        h.processor.process_one(Transfer{h.ids[0], h.ids[1], Money{25'000}});

    ASSERT_TRUE(result.ok()) << result.error();
    EXPECT_EQ(result.value().type, TransactionType::Transfer);
    EXPECT_EQ(balance_of(h.accounts, h.ids[0]), 100'000 - 25'000);
    EXPECT_EQ(balance_of(h.accounts, h.ids[1]), 100'000 + 25'000);

    const int64_t after_sum =
        balance_of(h.accounts, h.ids[0]) + balance_of(h.accounts, h.ids[1]);
    EXPECT_EQ(before_sum, after_sum);  // conservation

    // Exactly one record in each store, same id.
    ASSERT_EQ(h.transactions.all().size(), 1u);
    ASSERT_EQ(h.wal.load_all().size(), 1u);
    EXPECT_EQ(h.transactions.all()[0].id, result.value().id);
}

TEST(TransactionProcessor, FrozenAccountRejected)
{
    SyncHarness h;
    {
        Account frozen = *h.accounts.find_by_id(h.ids[0]);
        frozen.set_status(AccountStatus::Frozen);
        h.accounts.save(frozen);
    }

    const Result<TransactionRecord> result =
        h.processor.process_one(Withdraw{h.ids[0], Money{1}});

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(balance_of(h.accounts, h.ids[0]), 100'000);
}

TEST(TransactionProcessor, MissingAccountFailsCleanly)
{
    SyncHarness h;

    const Result<TransactionRecord> result =
        h.processor.process_one(Deposit{AccountId{9999}, Money{100}});

    ASSERT_TRUE(result.failed());
    EXPECT_NE(result.error().find("account not found"), std::string::npos);
    EXPECT_TRUE(h.transactions.all().empty());
}

TEST(TransactionProcessor, SubmitThenProcessUntilEmpty)
{
    SyncHarness h;

    auto id_a = h.processor.submit(Deposit{h.ids[0], Money{100}});
    auto id_b = h.processor.submit(Deposit{h.ids[0], Money{200}});
    auto id_c = h.processor.submit(Withdraw{h.ids[0], Money{50}});

    ASSERT_TRUE(id_a.ok()) << id_a.error();
    ASSERT_TRUE(id_b.ok()) << id_b.error();
    ASSERT_TRUE(id_c.ok()) << id_c.error();
    EXPECT_EQ(h.processor.pending(), 3u);

    const std::vector<Result<TransactionRecord>> results =
        h.processor.process_until_empty();

    ASSERT_EQ(results.size(), 3u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_TRUE(results[1].ok());
    EXPECT_TRUE(results[2].ok());

    // FIFO: reserved ids match execution order.
    EXPECT_EQ(results[0].value().id, id_a.value());
    EXPECT_EQ(results[1].value().id, id_b.value());
    EXPECT_EQ(results[2].value().id, id_c.value());

    EXPECT_EQ(h.processor.pending(), 0u);
    EXPECT_EQ(balance_of(h.accounts, h.ids[0]), 100'000 + 100 + 200 - 50);
    EXPECT_EQ(h.transactions.all().size(), 3u);
}

TEST(TransactionProcessor, SubmitAfterStopFails)
{
    SyncHarness h;
    h.processor.stop();

    const auto result = h.processor.submit(Deposit{h.ids[0], Money{100}});
    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.error(), "processor stopped");
    EXPECT_EQ(h.processor.pending(), 0u);
}

TEST(TransactionProcessor, WalFailureInsideLockLeavesStateIntact)
{
    InMemoryAccountRepository accounts;
    InMemoryTransactionRepository transactions;
    InMemoryAuditRepository audit;
    const std::vector<AccountId> ids = seed_accounts(accounts, 1, Money{100'000});

    TempDir dir{"procwal"};
    // Parent directory for the WAL does not exist: construction and every
    // append fail with an I/O error.
    WalLogger wal{dir.path / "missing_subdir" / "wal.log"};
    TransactionProcessor processor{0, accounts, transactions, audit, wal};

    const Result<TransactionRecord> result =
        processor.process_one(Deposit{ids[0], Money{500}});

    ASSERT_TRUE(result.failed());
    ASSERT_FALSE(result.error().empty());
    EXPECT_EQ(balance_of(accounts, ids[0]), 100'000);  // never applied
    EXPECT_TRUE(transactions.all().empty());

    AuditFilter filter;
    filter.action = "txn.failed";
    EXPECT_FALSE(audit.find(filter).empty());
}

TEST(TransactionProcessor, AuditEntriesWrittenForSuccess)
{
    SyncHarness h;

    const Result<TransactionRecord> result =
        h.processor.process_one(Transfer{h.ids[0], h.ids[1], Money{1000}});

    ASSERT_TRUE(result.ok()) << result.error();

    AuditFilter filter;
    filter.action = "txn.completed";
    const auto entries = h.audit.find(filter);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_NE(entries[0].details.find("transfer"), std::string::npos);
}

//
// Async shutdown suite — uses start()/stop() with real workers.
//

namespace {

// Async harness: 4 funded accounts, N pool workers.
struct AsyncHarness {
    AsyncHarness(std::size_t workers, std::size_t account_count)
        : dir{"async"},
          wal{dir.path / "wal.log"},
          processor{workers, accounts, transactions, audit, wal}
    {
        ids = seed_accounts(accounts, account_count, Money{100'000});
    }

    TempDir dir;
    InMemoryAccountRepository accounts;
    InMemoryTransactionRepository transactions;
    InMemoryAuditRepository audit;
    WalLogger wal;
    TransactionProcessor processor;
    std::vector<AccountId> ids;
};

} // namespace

TEST(TransactionProcessorShutdown, StopDrainsSubmittedTransactions)
{
    AsyncHarness h{2, 4};
    h.processor.start();

    constexpr int kCount = 20;
    std::vector<Result<TransactionId>> ids;
    ids.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        ids.push_back(h.processor.submit(
            Deposit{h.ids[static_cast<std::size_t>(i % 4)], Money{100}}));
    }
    for (const auto& id : ids) {
        ASSERT_TRUE(id.ok()) << id.error();
    }

    h.processor.stop();

    // Everything accepted before stop() must be fully committed.
    EXPECT_EQ(h.processor.pending(), 0u);
    ASSERT_EQ(h.transactions.all().size(), static_cast<std::size_t>(kCount));
    ASSERT_EQ(h.wal.load_all().size(), static_cast<std::size_t>(kCount));
    for (const auto& record : h.wal.load_all()) {
        EXPECT_EQ(record.state, TransactionState::Committed);
    }
}

TEST(TransactionProcessorShutdown, StopIdempotentAndDestructorSafe)
{
    auto h = std::make_unique<AsyncHarness>(2, 2);
    h->processor.start();
    ASSERT_TRUE(h->processor.submit(Deposit{h->ids[0], Money{1}}).ok());

    h->processor.stop();
    h->processor.stop();  // no crash / hang
    h.reset();            // destructor after explicit stop: no crash / hang

    // A fresh processor that is never stopped: destructor alone must stop it.
    auto h2 = std::make_unique<AsyncHarness>(2, 2);
    h2->processor.start();
    ASSERT_TRUE(h2->processor.submit(Deposit{h2->ids[0], Money{1}}).ok());
    h2.reset();
}

TEST(TransactionProcessorShutdown, StopFromNonWorkerThreadWhileBusy)
{
    auto h = std::make_shared<AsyncHarness>(2, 4);
    h->processor.start();

    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(h->processor
                        .submit(Deposit{h->ids[static_cast<std::size_t>(i % 4)],
                                        Money{1}})
                        .ok());
    }

    // Stop from this (non-worker) thread while consumers are chewing
    // through the backlog; must finish well within the watchdog.
    ASSERT_TRUE(completes_within([h] { h->processor.stop(); }, 5000ms))
        << "stop() did not finish while workers were busy";

    EXPECT_EQ(h->processor.pending(), 0u);
    EXPECT_EQ(h->transactions.all().size(), 100u);
}

TEST(TransactionProcessorShutdown, WaitPopTokenUnblocksStarvedWorker)
{
    AsyncHarness h{1, 1};
    h.processor.start();
    // Nothing submitted: the single consumer is blocked in wait_pop.
    std::this_thread::sleep_for(20ms);

    ASSERT_TRUE(completes_within([&h] { h.processor.stop(); }, 2000ms))
        << "starved consumer did not wake on stop";
}

TEST(TransactionProcessorShutdown, RestartAfterStopRebuildsWorkers)
{
    AsyncHarness h{2, 4};
    constexpr int kPerBatch = 5;

    // Batch 1: start, submit, stop.
    h.processor.start();
    for (int i = 0; i < kPerBatch; ++i) {
        ASSERT_TRUE(h.processor
                        .submit(Deposit{h.ids[static_cast<std::size_t>(i % 4)],
                                        Money{10}})
                        .ok());
    }
    h.processor.stop();
    ASSERT_EQ(h.processor.pending(), 0u);

    // Batch 2: start again (fresh pool), submit, stop again.
    h.processor.start();
    for (int i = 0; i < kPerBatch; ++i) {
        ASSERT_TRUE(h.processor
                        .submit(Withdraw{h.ids[static_cast<std::size_t>(i % 4)],
                                         Money{5}})
                        .ok());
    }
    h.processor.stop();

    // All 10 records committed across both cycles.
    EXPECT_EQ(h.processor.pending(), 0u);
    ASSERT_EQ(h.transactions.all().size(), static_cast<std::size_t>(2 * kPerBatch));
    ASSERT_EQ(h.wal.load_all().size(), static_cast<std::size_t>(2 * kPerBatch));
    for (const auto& record : h.wal.load_all()) {
        EXPECT_EQ(record.state, TransactionState::Committed);
    }

    // Batch 1 deposits 10 to accounts i%4 (i=0..4); batch 2 withdraws 5
    // from the same pattern. Accounts 0 and 1 are hit twice, 2 and 3 once.
    for (std::size_t idx = 0; idx < h.ids.size(); ++idx) {
        const auto account = h.accounts.find_by_id(h.ids[idx]);
        ASSERT_TRUE(account.has_value());
        int hits = 0;
        for (int i = 0; i < kPerBatch; ++i) {
            if (static_cast<std::size_t>(i % 4) == idx) {
                ++hits;
            }
        }
        EXPECT_EQ(account->balance().cents(), 100'000 + hits * 10 - hits * 5);
    }
}

TEST(TransactionProcessorShutdown, SubmitAfterStopThenRestartSucceeds)
{
    AsyncHarness h{2, 2};
    h.processor.start();
    h.processor.stop();

    // Stopped: submit must be rejected, not silently queued or crashed.
    const Result<TransactionId> rejected =
        h.processor.submit(Deposit{h.ids[0], Money{1}});
    ASSERT_FALSE(rejected.ok());
    EXPECT_NE(rejected.error().find("processor stopped"), std::string::npos);
    EXPECT_EQ(h.transactions.all().size(), 0u);

    // Restart: submit must succeed again.
    h.processor.start();
    const Result<TransactionId> accepted =
        h.processor.submit(Deposit{h.ids[0], Money{1}});
    EXPECT_TRUE(accepted.ok()) << accepted.error();
    h.processor.stop();

    EXPECT_EQ(h.transactions.all().size(), 1u);
}
