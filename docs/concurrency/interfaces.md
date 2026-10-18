# 1. `docs/concurrency/interfaces.md`

## Namespace, conventions, and prerequisites

- All new types live in `namespace bank`, files `#pragma once`, project includes in quotes root-relative from `src/` (e.g. `#include "domain/Types.h"`), std includes in brackets — matching existing style.
- **Prerequisite edit (flagged):** `AccountLockManager` stores mutexes in `std::unordered_map<AccountId, ...>`, which requires a `std::hash` specialization for the strong typedef `bank::Id<Tag>`. A partial specialization `namespace std { template <typename Tag> struct hash<bank::Id<Tag>>; }` must be added to `src/domain/Types.h` (returning `std::hash<uint64_t>{}(id.value())`). This is the only planned edit outside `src/concurrency/` and `tests/`.
- `TransactionState` must not be reordered (WAL on-disk schema); the concurrency code only *reads* its values.

## Ownership map

| Owner | Owns |
|---|---|
| `ThreadPool` | `std::vector<std::jthread>` (the workers); its task queue; its `std::mutex` + `std::condition_variable_any` |
| `TransactionQueue` | a `std::queue<Transaction>`; its `std::mutex` + `std::condition_variable_any` (queue is self-contained, non-copyable, non-movable) |
| `AccountLockManager` | `std::unordered_map<AccountId, std::unique_ptr<std::mutex>>` — the per-account mutexes live **here**, never in `Account`; a `map_mutex_` guarding the map itself |
| `ScopedAccountLock` | nothing; holds a non-owning `std::mutex*` it locked in its constructor and unlocks in its destructor (move-only) |
| `ScopedAccountLocks` | two member `ScopedAccountLock`s acquired in ascending-`AccountId` order (second is `std::optional` for the `a == b` case) |
| `TransactionProcessor` | **by value:** `TransactionQueue queue_`, `AccountLockManager locks_`, `ThreadPool pool_`, an `std::stop_source shutdown_`, an id counter. **Non-owning references:** `AccountRepository&`, `TransactionRepository&`, `AuditRepository&`, `WalLogger&` — injected so tests can pass `InMemory*` repos and a temp-dir `WalLogger` |

`Account` is untouched: no mutex, no lock state. `Account::version_` stays reserved; optimistic-retry on `save()` is **not** in this milestone (the processor serializes writes under the account mutex, so no lost updates occur).

## `concurrency/ScopedAccountLock.h` (header-only)

```cpp
#pragma once

#include <mutex>

#include "domain/Types.h"

namespace bank {

// RAII guard for exactly one account mutex. Move-only.
class ScopedAccountLock {
public:
    // Step-1 standalone form: locks `mutex` immediately.
    explicit ScopedAccountLock(std::mutex& mutex);

    // Locked in step 2: acquires `manager`'s mutex for `id` (creating it
    // on demand under the manager's map mutex).
    ScopedAccountLock(AccountLockManager& manager, AccountId id);

    ~ScopedAccountLock();

    ScopedAccountLock(const ScopedAccountLock&) = delete;
    ScopedAccountLock& operator=(const ScopedAccountLock&) = delete;
    ScopedAccountLock(ScopedAccountLock&& other) noexcept;
    ScopedAccountLock& operator=(ScopedAccountLock&& other) noexcept;

    [[nodiscard]] AccountId id() const noexcept;  // kInvalidAccountId in step-1 form

    ScopedAccountLock() = delete;

private:
    friend class AccountLockManager;
    friend class ScopedAccountLocks;

    std::mutex* mutex_{nullptr};
    AccountId id_{};
};

} // namespace bank
```

Forward declaration of `AccountLockManager` needed in this header (`class AccountLockManager;`) for the step-2 constructor.

## `concurrency/ScopedAccountLocks.h` (header-only)

```cpp
#pragma once

#include <array>
#include <optional>

#include "concurrency/ScopedAccountLock.h"
#include "domain/Types.h"

namespace bank {

class AccountLockManager;

// RAII guard for two account mutexes, always acquired in ascending
// AccountId order. If first == second, exactly one mutex is held.
// Non-copyable, non-movable (lock order must not be disturbed by moves).
class ScopedAccountLocks {
public:
    ScopedAccountLocks(AccountLockManager& manager, AccountId first, AccountId second);
    ~ScopedAccountLocks();

    ScopedAccountLocks(const ScopedAccountLocks&) = delete;
    ScopedAccountLocks& operator=(const ScopedAccountLocks&) = delete;
    ScopedAccountLocks(ScopedAccountLocks&&) = delete;
    ScopedAccountLocks& operator=(ScopedAccountLocks&&) = delete;

    // ids() are sorted ascending; size() is 1 when first == second.
    [[nodiscard]] const std::array<AccountId, 2>& ids() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::array<AccountId, 2> ids_{};                 // ids_[0] <= ids_[1]
    std::optional<ScopedAccountLock> first_;         // lower id
    std::optional<ScopedAccountLock> second_;        // higher id, absent if ids equal
};

} // namespace bank
```

## `concurrency/AccountLockManager.h`

```cpp
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "concurrency/ScopedAccountLock.h"
#include "concurrency/ScopedAccountLocks.h"
#include "domain/Types.h"

namespace bank {

// Owns one std::mutex per AccountId, created on first use.
// Non-copyable, non-movable.
class AccountLockManager {
public:
    AccountLockManager() = default;
    ~AccountLockManager() = default;

    AccountLockManager(const AccountLockManager&) = delete;
    AccountLockManager& operator=(const AccountLockManager&) = delete;
    AccountLockManager(AccountLockManager&&) = delete;
    AccountLockManager& operator=(AccountLockManager&&) = delete;

    [[nodiscard]] ScopedAccountLock lock(AccountId id);

    // Locks both in ascending id order (see ScopedAccountLocks).
    [[nodiscard]] ScopedAccountLocks lock(AccountId first, AccountId second);

    // Number of mutexes created so far (test/diagnostic use).
    [[nodiscard]] std::size_t mutex_count() const;

private:
    friend class ScopedAccountLock;
    friend class ScopedAccountLocks;

    // Returns the mutex for `id`, creating it if absent.
    // Caller must NOT already hold map_mutex_.
    [[nodiscard]] std::mutex& mutex_for(AccountId id);

    mutable std::mutex map_mutex_;
    std::unordered_map<AccountId, std::unique_ptr<std::mutex>> mutexes_;
};

} // namespace bank
```

## `concurrency/TransactionQueue.h`

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>

#include "domain/Transaction.h"

namespace bank {

// Unbounded FIFO of Transactions. Thread-safe.
// Uses condition_variable_any so waits can be interrupted by a stop_token.
// Non-copyable, non-movable.
class TransactionQueue {
public:
    TransactionQueue() = default;
    ~TransactionQueue() = default;

    TransactionQueue(const TransactionQueue&) = delete;
    TransactionQueue& operator=(const TransactionQueue&) = delete;
    TransactionQueue(TransactionQueue&&) = delete;
    TransactionQueue& operator=(TransactionQueue&&) = delete;

    // Returns false once close() has been called.
    bool enqueue(Transaction txn);

    // Blocks until an item is available, or returns nullopt when the queue
    // is closed AND empty, or when `token` is stopped and the queue is
    // observed empty. FIFO order.
    [[nodiscard]] std::optional<Transaction> wait_pop(const std::stop_token& token);

    // Non-blocking variant; nullopt if empty (even if open).
    [[nodiscard]] std::optional<Transaction> try_pop();

    // Idempotent. Wakes all waiters; subsequent enqueue() fails;
    // wait_pop() drains the remainder, then returns nullopt.
    void close();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool closed() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::queue<Transaction> queue_;
    bool closed_{false};
};

} // namespace bank
```

## `concurrency/ThreadPool.h`

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace bank {

// Fixed-size pool of std::jthread workers.
// Tasks are std::function (must be copy-constructible; C++20 has no
// std::move_only_function). Non-copyable, non-movable.
class ThreadPool {
public:
    // thread_count == 0 is legal: submit() still queues, stop() still drains
    // synchronously — useful for deterministic single-threaded tests.
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();  // calls stop()

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Returns false once stop() has begun (no new work accepted).
    bool submit(std::function<void()> task);

    // Orderly shutdown: (1) flip stopping_ so submit() refuses new work,
    // (2) request_stop + notify_all, (3) workers drain the task queue
    // completely, then exit, (4) join all jthreads.
    // Idempotent. Must NOT be called from a pool worker thread
    // (it would self-join; precondition, asserted in debug).
    void stop();

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t pending() const;  // tasks queued, not yet started

private:
    void worker(std::stop_token token);

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    bool stopping_{false};
};

} // namespace bank
```

Worker loop contract (implemented in the .cpp): `wait(lock, token, []{ return !tasks_.empty() || stopping_; })`; if `stopping_ && tasks_.empty()` → exit; else pop one task, unlock, run it. Draining is therefore "finish every task already submitted", matching `stop()`'s step 3.

## `concurrency/TransactionProcessor.h`

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
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

// Serializes transaction execution: TransactionQueue -> ThreadPool workers
// -> per-account locks -> validate -> WAL intent -> apply -> WAL commit.
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

    // Assigns a fresh TransactionId, enqueues the transaction.
    // Fails ("processor stopped") once stop() has begun.
    // Pure enqueue — does NOT wait for execution.
    [[nodiscard]] Result<TransactionId> submit(Transaction txn);

    // Synchronous path (tests, and the worker body): pops nothing;
    // executes ONE transaction end-to-end under account locks.
    // Never throws: domain errors and WalLogger failures become
    // Result::failure with a non-empty message.
    [[nodiscard]] Result<TransactionRecord> process_one();

    // Repeatedly calls process_one() until the queue is empty.
    // Returns one Result per transaction processed (in FIFO order).
    [[nodiscard]] std::vector<Result<TransactionRecord>> process_until_empty();

    // Launches `worker_count` long-running consumers on the pool.
    // Each consumer loops: wait_pop(shutdown_token) -> process_one.
    // Idempotent (second start() is a no-op).
    void start();

    // Shutdown semantics (see decisions.md §6):
    //   1. accepting_ = false  (submit() starts failing)
    //   2. shutdown_.request_stop()
    //   3. queue_.close()      (wakes consumers; they drain then exit)
    //   4. pool_.stop()        (joins the consumer tasks)
    // Idempotent; may be called from any thread that is not a pool
    // worker of this processor's pool.
    void stop();

    // Transactions accepted but not yet fully processed.
    [[nodiscard]] std::size_t pending() const;

private:
    // The full critical section for one transaction (see decisions.md §3):
    // lock accounts -> load -> validate -> WAL intent (Validated) ->
    // apply -> save accounts -> WAL commit (Committed) -> txn repo append
    // -> audit append -> unlock (RAII).
    // Catches std::exception from repos/WalLogger and maps to failure.
    [[nodiscard]] Result<TransactionRecord> process_impl(const Transaction& txn,
                                                         TransactionId id);

    [[nodiscard]] void append_audit(CustomerId actor, std::string_view action,
                                    std::string_view details);

    TransactionQueue queue_;
    AccountLockManager locks_;
    ThreadPool pool_;
    std::stop_source shutdown_;

    AccountRepository& accounts_;
    TransactionRepository& transactions_;
    AuditRepository& audit_;
    WalLogger& wal_;

    std::atomic<uint64_t> next_id_{1};
    std::atomic<bool> accepting_{false};
};

} // namespace bank
```

Wiring at construction: `pool_(worker_count)`, id counter seeded in the constructor body from `max(id over transactions_.all(), id over wal_.load_all()) + 1` so ids never collide with pre-existing records.

## Complete `#include` list per header

| Header | Includes |
|---|---|
| `ScopedAccountLock.h` | `<mutex>`; `"domain/Types.h"`; forward-declares `AccountLockManager` |
| `ScopedAccountLocks.h` | `<array>`, `<optional>`; `"concurrency/ScopedAccountLock.h"`, `"domain/Types.h"`; forward-declares `AccountLockManager` |
| `AccountLockManager.h` | `<cstddef>`, `<memory>`, `<mutex>`, `<unordered_map>`; `"concurrency/ScopedAccountLock.h"`, `"concurrency/ScopedAccountLocks.h"`, `"domain/Types.h"` |
| `TransactionQueue.h` | `<condition_variable>`, `<cstddef>`, `<mutex>`, `<optional>`, `<queue>`, `<stop_token>`; `"domain/Transaction.h"` |
| `ThreadPool.h` | `<condition_variable>`, `<cstddef>`, `<functional>`, `<mutex>`, `<queue>`, `<stop_token>`, `<thread>`, `<vector>` |
| `TransactionProcessor.h` | `<atomic>`, `<cstddef>`, `<cstdint>`, `<stop_token>`, `<string>`, `<string_view>`, `<vector>`; `"concurrency/AccountLockManager.h"`, `"concurrency/ThreadPool.h"`, `"concurrency/TransactionQueue.h"`, `"core/Result.h"`, `"domain/Account.h"`, `"domain/Transaction.h"`, `"persistence/AccountRepository.h"`, `"persistence/AuditRepository.h"`, `"persistence/TransactionRepository.h"`, `"persistence/WalLogger.h"` |

`.cpp` files (`ThreadPool.cpp`, `TransactionQueue.cpp`, `AccountLockManager.cpp`, `TransactionProcessor.cpp`) add `<algorithm>` (for `std::sort` on the two ids) and `"domain/TransactionOps.h"` / `"domain/AuditEntry.h"` / `"core/TimeUtil.h"` / `<exception>` as needed.

## CMake note (no changes in this pass)

- `bank` target: add `src/concurrency/{ThreadPool,TransactionQueue,AccountLockManager,TransactionProcessor}.cpp` (explicit list, matching existing style; the two lock headers are header-only).
- `bank_tests` target: add `tests/{test_thread_pool,test_transaction_queue,test_account_locks,test_transaction_processor,test_stress}.cpp`.
- `Threads::Threads` is already `PUBLIC` on `bank` — nothing else needed.
