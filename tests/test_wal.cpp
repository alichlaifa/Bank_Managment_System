#include <doctest.h>

#include "TestSupport.h"

#include "domain/Transaction.h"
#include "domain/TransactionOps.h"
#include "persistence/SnapshotStore.h"
#include "persistence/WalLogger.h"

#include <algorithm>
#include <fstream>

using namespace bank;

namespace {

TransactionRecord deposit_record(TransactionId id, AccountId account, int64_t cents)
{
    return project_transaction(id, Deposit{account, Money{cents}});
}

TransactionRecord withdraw_record(TransactionId id, AccountId account, int64_t cents)
{
    return project_transaction(id, Withdraw{account, Money{cents}});
}

TransactionRecord transfer_record(TransactionId id, AccountId from, AccountId to, int64_t cents)
{
    return project_transaction(id, Transfer{from, to, Money{cents}});
}

} // namespace

TEST_CASE("append and commit record a single transaction")
{
    TempDir dir{"tmp"};
    const auto path = dir.path / "wal.log";

    {
        WalLogger wal{path};
        wal.append(deposit_record(TransactionId{1}, AccountId{1}, 500));
        wal.mark_committed(TransactionId{1});
        REQUIRE(wal.size() == 1);
    }
    {
        WalLogger wal{path};
        REQUIRE(wal.size() == 1);
        const auto records = wal.load_all();
        REQUIRE(records.size() == 1);
        REQUIRE(records[0].id == TransactionId{1});
        REQUIRE(records[0].state == TransactionState::Committed);
        REQUIRE(records[0].amount == Money{500});
    }
}

TEST_CASE("mark_committed is idempotent")
{
    TempDir dir{"tmp"};
    WalLogger wal{dir.path / "wal.log"};
    wal.append(deposit_record(TransactionId{1}, AccountId{1}, 10));
    wal.mark_committed(TransactionId{1});
    wal.mark_committed(TransactionId{1});
    REQUIRE(wal.size() == 1);
    REQUIRE(wal.load_all().front().state == TransactionState::Committed);
}

TEST_CASE("mark_committed on unknown id throws logic_error")
{
    TempDir dir{"tmp"};
    WalLogger wal{dir.path / "wal.log"};
    REQUIRE_THROWS_AS(wal.mark_committed(TransactionId{99}), std::logic_error);
}

TEST_CASE("replay_from counts by logical record index, not transaction id")
{
    TempDir dir{"tmp"};
    WalLogger wal{dir.path / "wal.log"};
    wal.append(deposit_record(TransactionId{1}, AccountId{1}, 100));
    wal.mark_committed(TransactionId{1});
    wal.append(withdraw_record(TransactionId{2}, AccountId{1}, 25));
    wal.mark_committed(TransactionId{2});

    const auto from_start = wal.replay_from(0);
    REQUIRE(from_start.size() == 2);

    const auto from_second = wal.replay_from(1);
    REQUIRE(from_second.size() == 1);
    REQUIRE(from_second[0].id == TransactionId{2});

    const auto beyond = wal.replay_from(100);
    REQUIRE(beyond.empty());
}

TEST_CASE("uncommitted record is not marked committed in the log")
{
    TempDir dir{"tmp"};
    const auto path = dir.path / "wal.log";
    {
        WalLogger wal{path};
        wal.append(deposit_record(TransactionId{1}, AccountId{1}, 50));
        wal.mark_committed(TransactionId{1});
        wal.append(deposit_record(TransactionId{2}, AccountId{1}, 70)); // never committed
    }

    // Simulates a crash: the log has two records but only one is committed.
    WalLogger wal{path};
    const auto records = wal.replay_from(0);
    REQUIRE(records.size() == 2);

    const std::size_t committed_count = std::count_if(
        records.begin(), records.end(),
        [](const TransactionRecord& r) { return r.state == TransactionState::Committed; });
    REQUIRE(committed_count == 1);
}

TEST_CASE("corrupt record lines are silently skipped during replay")
{
    TempDir dir{"tmp"};
    const auto path = dir.path / "wal.log";
    {
        std::ofstream out{path, std::ios::binary};
        // The header state byte is written as '0' + int(state); '4' is Committed.
        const std::string good_line =
            "0000000000000001" +
            std::string(1, static_cast<char>('0' + static_cast<int>(TransactionState::Committed))) +
            R"({"id":1,"ts":0,"type":"deposit","state":"committed","from":null,"to":1,"amount":500})" + '\n';
        const std::string bad_line =
            "0000000000000002" + std::string(1, '4') + "{{{{bad json}}}}\n";
        out << good_line << bad_line;
    }

    WalLogger wal{path};
    REQUIRE(wal.size() == 2);
    const auto records = wal.load_all();
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].id == TransactionId{1});
}

TEST_CASE("snapshot store round-trips and overwrites atomically")
{
    TempDir dir{"tmp"};
    SnapshotStore snapshots{dir.path / "snapshot.txt"};
    REQUIRE_FALSE(snapshots.load().has_value());

    snapshots.store(Snapshot{
        .lsn = 7,
        .accounts = {Account{AccountId{1}, CustomerId{1}, Money{100},
                     AccountStatus::Active, 2}}
    });

    const auto loaded = snapshots.load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->lsn == 7);
    REQUIRE(loaded->accounts.size() == 1);
    REQUIRE(loaded->accounts[0].balance() == Money{100});
    REQUIRE(loaded->accounts[0].version() == 2);

    // Overwrite on the same path must succeed cleanly.
    snapshots.store(Snapshot{.lsn = 8, .accounts = {}});
    const auto overwritten = snapshots.load();
    REQUIRE(overwritten.has_value());
    REQUIRE(overwritten->lsn == 8);
    REQUIRE(overwritten->accounts.empty());
}

TEST_CASE("full crash-recovery: snapshot replay restores exact balances")
{
    TempDir dir{"tmp"};
    const auto wal_path    = dir.path / "wal.log";
    const auto snap_path   = dir.path / "snapshot.txt";

    std::vector<Account> live{
        Account{AccountId{1}, CustomerId{100}, Money{100'000}, AccountStatus::Active, 0},
        Account{AccountId{2}, CustomerId{100}, Money{200'000}, AccountStatus::Active, 0},
    };

    {
        WalLogger wal{wal_path};
        SnapshotStore snapshots{snap_path};

        // Commit two transactions, checkpoint, then commit one more.
        wal.append(deposit_record(TransactionId{1}, AccountId{1}, 5'000));
        txn::apply(materialize_transaction(wal.load_all().back()), std::span<Account>{live});
        wal.mark_committed(TransactionId{1});

        wal.append(withdraw_record(TransactionId{2}, AccountId{2}, 25'000));
        txn::apply(materialize_transaction(wal.load_all().back()), std::span<Account>{live});
        wal.mark_committed(TransactionId{2});

        snapshots.store(Snapshot{.lsn = wal.size(), .accounts = live});

        wal.append(transfer_record(TransactionId{3}, AccountId{1}, AccountId{2}, 10'000));
        txn::apply(materialize_transaction(wal.load_all().back()), std::span<Account>{live});
        wal.mark_committed(TransactionId{3});
    }

    // --- Begin recovery ---
    auto snapshot = SnapshotStore{snap_path}.load();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->lsn == 2);

    WalLogger wal{wal_path};
    std::vector<Account> recovered = snapshot->accounts;

    for (const auto& record : wal.replay_from(snapshot->lsn)) {
        REQUIRE(record.state == TransactionState::Committed);
        txn::apply(materialize_transaction(record), std::span<Account>{recovered});
    }

    REQUIRE(recovered[0].balance() == Money{95'000});   // 100k + 5k - 10k
    REQUIRE(recovered[1].balance() == Money{185'000});  // 200k - 25k + 10k
}