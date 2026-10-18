#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "concurrency/AccountLockManager.h"
#include "concurrency/ThreadPool.h"
#include "concurrency/TransactionQueue.h"
#include "core/Result.h"
#include "domain/Account.h"
#include "domain/Transaction.h"
#include "persistence/AccountRepository.h"
#include "persistence/AuditRepository.h"
#include "persistence/TransactionRepository.h"
#include "persistence/WalLogger.h"

namespace bank {

// Serializes transaction execution: TransactionQueue -> ThreadPool consumers
// -> per-account locks -> validate -> WAL intent -> apply -> WAL commit.
//
// Lock order inside process_impl: account mutex(es) -> WalLogger mutex (taken
// internally by WalLogger) -> repos_mutex_ (the InMemory repositories are not
// thread-safe; every repo access is serialized here). Never acquire an
// account lock while holding repos_mutex_. submit_mutex_ is a leaf with
// respect to the others except that submit() takes the queue mutex while
// holding it; consumers never take submit_mutex_.
//
// Shutdown contract (decisions.md §6): stop() refuses new submits first
// (accepting_ = false under submit_mutex_), then request_stop() wakes the
// consumers; each consumer drains the queue (wait_pop returns items while
// non-empty even with a stopped token) and exits when it is empty;
// pool_->stop() joins them. The queue itself is never closed by the
// processor, so start() may be called again after stop() (each cycle gets a
// fresh pool and stop source). Idempotent. Must not be called from a
// consumer of this processor's pool.
//
// Non-copyable, non-movable.
class TransactionProcessor {
public:
    TransactionProcessor(std::size_t worker_count,
                         AccountRepository& accounts,
                         TransactionRepository& transactions,
                         AuditRepository& audit,
                         WalLogger& wal);
    ~TransactionProcessor();  // calls stop() if not already stopped

    TransactionProcessor(const TransactionProcessor&) = delete;
    TransactionProcessor& operator=(const TransactionProcessor&) = delete;
    TransactionProcessor(TransactionProcessor&&) = delete;
    TransactionProcessor& operator=(TransactionProcessor&&) = delete;

    // Enqueues a transaction. Pure enqueue — does NOT wait for execution.
    // The returned id is the id the persisted record will carry (reserved at
    // submit time, FIFO). Fails ("processor stopped") once stop() has begun.
    [[nodiscard]] Result<TransactionId> submit(Transaction txn);

    // Synchronous path: executes ONE specific transaction end-to-end under
    // account locks. Does not touch the queue. Never throws: domain errors
    // and WalLogger/repo failures become Result::failure with a non-empty
    // message. The returned record carries a fresh TransactionId and
    // state == Committed on success.
    [[nodiscard]] Result<TransactionRecord> process_one(const Transaction& txn);

    // Pops one transaction from the queue and processes it; nullopt when the
    // queue is empty.
    [[nodiscard]] std::optional<Result<TransactionRecord>> process_next();

    // Repeatedly process_next() until the queue is empty. One Result per
    // transaction, in FIFO order.
    [[nodiscard]] std::vector<Result<TransactionRecord>> process_until_empty();

    // Launches worker_count long-running consumers on the internal pool.
    // Each consumer loops: wait_pop(shutdown token) -> process.
    // Idempotent while running; may be called again after stop() (each
    // start/stop cycle gets a fresh pool and stop source).
    void start();

    // See the shutdown contract above. Idempotent.
    void stop();

    // Transactions queued but not yet popped by a consumer / process_next.
    [[nodiscard]] std::size_t pending() const;

private:
    // Full critical section for one transaction under account locks.
    // Catches std::exception and maps to Result::failure; never throws.
    [[nodiscard]] Result<TransactionRecord> process_impl(const Transaction& txn,
                                                         TransactionId id);

    // Long-running consumer body run on the pool: wait_pop -> reserve id ->
    // process_impl, until wait_pop reports shutdown + empty.
    void consumer_loop();

    // Pops the id reserved by submit() for the next queued transaction, or
    // assigns a fresh one when called outside the submit path.
    [[nodiscard]] TransactionId take_reserved_id();

    void append_audit(CustomerId actor, std::string_view action,
                      std::string_view details);

    TransactionQueue queue_;
    AccountLockManager locks_;
    std::unique_ptr<ThreadPool> pool_;  // created by start(), destroyed by stop()
    std::stop_source shutdown_;

    AccountRepository& accounts_;
    TransactionRepository& transactions_;
    AuditRepository& audit_;
    WalLogger& wal_;

    mutable std::mutex repos_mutex_;   // serializes all repository access
    std::mutex submit_mutex_;          // guards accepting_/started_/pool_/shutdown_
    mutable std::mutex reserved_mutex_;  // guards reserved_
    std::queue<TransactionId> reserved_; // ids handed out by submit(), FIFO
    std::atomic<uint64_t> next_id_{1};
    std::atomic<uint64_t> next_audit_id_{1};
    std::atomic<bool> accepting_{true};
    bool started_{false};  // guarded by submit_mutex_
    std::size_t worker_count_{0};
};

} // namespace bank
