# 3. `docs/concurrency/tests.md`

All tests are plain GTest (`TEST(Suite, Case)`), no fixtures required except where noted, no external deps. Existing style: `using namespace bank;`, anonymous-namespace helpers, assertions on `Result::ok()`/`error()`.

## Categories and cases

### A. Unit — `ScopedAccountLock` (`tests/test_account_locks.cpp`, suite `ScopedAccountLock`)

| Test | Invariant | How it fails if code is wrong |
|---|---|---|
| `LocksOnConstructionUnlocksOnDestruction` | A `std::mutex` locked via a guard blocks a second `try_lock` until the guard dies | If dtor forgets `unlock()`, `try_lock` fails after scope exit → assertion fails; if ctor forgets `lock()`, `try_lock` from another thread succeeds while guard is alive → assertion fails |
| `MoveTransfersOwnership` | Moved-from guard holds nothing; moved-to guard unlocks exactly once | Double-unlock → UB caught by ASan/dev; missed unlock → post-scope `try_lock` fails |
| `DoubleLockSameMutexDeadlocksUnlessSameThread` | Documented UB avoided: test locks one mutex with two guards **in the same thread is forbidden** — instead test asserts `mutex.try_lock()` from a second thread fails while guard alive, succeeds after | Wrong unlock count → `try_lock` post-condition flips |

### B. Unit — `AccountLockManager` (same file, suite `AccountLockManager`)

| Test | Invariant | How it fails if code is wrong |
|---|---|---|
| `LockSingleAccountMutualExclusion` | Two threads, each loops `guard = mgr.lock(id); probe.enter()/leave()`; probe's atomic depth never exceeds 1 | Missing lock → probe records depth 2 → `ASSERT` fails |
| `LockTwoAccountsSortsAscending` | `lock(b, a)` with b > a: while first thread holds the guard, second thread's `lock(a, c)` etc. — **direct check:** expose nothing; instead run `lock(hi, lo)` and `lock(lo, hi)` concurrently on a barrier in a loop of 1'000 iterations with a watchdog (see §D). Sorted implementation completes; unsorted ABBA deadlocks → watchdog fires | Deadlock → test times out via `completes_within` helper → explicit `FAIL()` with message |
| `LockSameAccountTwiceYieldsOneMutex` | `lock(a, a).size() == 1`; `mutex_count()` increased by exactly 1 | Wrong size → assertion; double-lock same mutex → self-deadlock → watchdog fires |
| `MutexesAreCreatedOnDemand` | `mutex_count()` is 0 initially, grows on first lock of each id, stable on re-lock | Leak/regrow → count assertion fails |
| `DistinctAccountsHaveDistinctMutexes` | Thread 1 holds `lock(a)`; thread 2's `lock(b)` completes (does not block) | If striped/shared mutex were used, thread 2 blocks → `completes_within` fails |

### C. Unit — `TransactionQueue` (`tests/test_transaction_queue.cpp`, suite `TransactionQueue`)

| Test | Invariant | How it fails if code is wrong |
|---|---|---|
| `FifoOrder` | Push Deposit(a,1), Deposit(a,2), Withdraw(a,3); `try_pop` thrice yields that exact order (compare via `txn::amount_of` / `std::get`) | Wrong container/order → mismatch |
| `TryPopEmptyReturnsNullopt` | Empty open queue → `try_pop() == nullopt` | Wrong → non-empty optional |
| `EnqueueAfterCloseFails` | `close()` then `enqueue` → `false`; size unchanged | Missing closed check → true |
| `WaitPopDrainsThenNulloptAfterClose` | Push 2, `close()`, two `wait_pop`s yield the items, third yields `nullopt` | Premature nullopt → lost item assertion; never nullopt → hang (watchdog via jthread + latch) |
| `WaitPopWokenByStopToken` | Empty open queue; `std::jthread` blocks in `wait_pop(token)`; main sets a `std::latch`, thread signals it *before* waiting? — instead: main calls `stop_source.request_stop()` after observing the thread entered via a second latch placed immediately before the wait call; wait returns `nullopt` within timeout | Without token support in the wait, thread hangs → `latch.wait_for` timeout → fail |
| `SizeAndClosedQueries` | `size()`/`closed()` reflect state | Off-by-one → assertion |

### D. Unit + deadlock — `ThreadPool` (`tests/test_thread_pool.cpp`, suite `ThreadPool`)

| Test | Invariant | How it fails if code is wrong |
|---|---|---|
| `TasksExecute` | Submit 8 tasks incrementing one `std::atomic<int>`; after `stop()`, count == 8 | Lost wakeups/dropped tasks → count < 8 |
| `PendingCountsUnstartedTasks` | `pending()` reflects queued-but-unstarted count (checked with 0-thread pool for determinism) | Wrong counter → assertion |
| `SubmitAfterStopReturnsFalse` | `stop()` then `submit` → false | Missing flag → true |
| `StopIsIdempotent` | `stop(); stop();` no crash/hang | Double-join → crash (ASan/TSan catch) |
| `StopDrainsQueue` | 0-thread pool: submit 5 recording tasks, `stop()`, vector has all 5 in order | Early exit before drain → missing entries |
| `ZeroThreadPoolRunsNothingUntilStop` | With `ThreadPool(0)`, submitted tasks do not run until `stop()` drains them synchronously | Premature execution → order assertion |
| `ManyThreadsManyTasks` (TSan) | 4 threads × 250 tasks each into one pool; total == 1000; runs clean under TSan | Data race in queue → TSan report (non-zero exit); lost tasks → count |

### E. Unit — `TransactionProcessor` single-threaded (`tests/test_transaction_processor.cpp`, suite `TransactionProcessor`)

Fixture-style helper (in anonymous namespace, not `TEST_F` — matching project style): builds `InMemoryAccountRepository` with N seeded accounts, `InMemoryTransactionRepository`, `InMemoryAuditRepository`, `WalLogger` in a `TempDir`, and a `TransactionProcessor{0 /*workers*/, ...}` so everything runs via `process_one`/`process_until_empty` deterministically.

| Test | Invariant | How it fails if code is wrong |
|---|---|---|
| `DepositIncreasesBalanceAndCommits` | After `process_one`, repo balance += amount; returned record `state == Committed`; WAL `load_all()` has one committed record; txn repo has one record | Wrong state machine → state mismatch; skipped WAL/repo write → missing records |
| `WithdrawInsufficientFundsFails` | `process_one().failed()`; error contains "insufficient funds" and is non-empty; balance unchanged; WAL has **no committed** record for it; audit has a `txn.failed` entry | Applying before validate → balance changed → assertion; empty error string → explicit `ASSERT_FALSE(error().empty())` (the Result<void> trap) |
| `TransferMovesFundsAtomically` | from −X, to +X, both records consistent; single transaction id in both repos | Partial apply → sum of balances changes → conservation assertion |
| `FrozenAccountRejected` | Withdraw on Frozen → failure; balance unchanged | Status check skipped → success where failure expected |
| `MissingAccountFailsCleanly` | Unknown id → `"account not found"` failure, no throw | Throw escapes → GTest catches as failure |
| `SubmitThenProcessUntilEmpty` | `submit` × 3, `pending() == 3`, `process_until_empty()` returns 3 successes FIFO | Wrong wiring of queue → empty vector / wrong order |
| `SubmitAfterStopFails` | `stop()` then `submit` → failure `"processor stopped"` | Missing accepting_ check |
| `WalFailureInsideLockLeavesStateIntact` | Use a `WalLogger` pointed at an **unwritable path** (TempDir + a path under a non-existent directory for portability): `process_one` fails with `"wal append failed"`; account balance unchanged; audit entry written | Applied-before-WAL → balance changed; uncaught exception → test crash |
| `AuditEntriesWrittenForSuccess` | Successful transfer appends audit entries with action strings | Missing append → empty audit repo |

### F. Deterministic interleavings (std::latch / std::barrier) — where and why

Determinism is required exactly where wall-clock scheduling could let a "bad" implementation accidentally pass:

1. **Mutual exclusion under contention** (`test_account_locks.cpp`): Thread A `latchA.count_down()` *while holding* the account lock, then waits on `latchB`; main waits `latchA`, then spawns/lets thread B attempt the same lock; B must **not** enter before `latchB.count_down()`. Verified with an atomic "in critical section" probe plus a `wait_for` on B's completion future with a short timeout *while A still holds*: if B enters early (no lock) the probe trips; if B never proceeds after release the lock is leaked. A plain loop test could pass by luck; the latch pins the exact interleaving.
2. **Shutdown drains in-flight work** (`test_transaction_queue.cpp` / `test_thread_pool.cpp`): a consumer is proven to be *inside* `wait_pop` (latch counted down on the line before the call) before `close()`/`request_stop()` fires. Without this, the wait might not yet be blocked and the test wouldn't actually exercise the wakeup path.
3. **ABBA transfer stress seed** (`test_stress.cpp`): a `std::barrier` releases two threads simultaneously — one running transfer A→B, one B→A — maximizing the chance an unsorted lock implementation deadlocks within a small iteration count. The barrier is what makes the race window reliable rather than probabilistic.

`std::latch` (`<latch>`) for one-shot handoffs; `std::barrier` (`<barrier>`) for the simultaneous-release case. No external sync deps.

### G. Shutdown tests (`test_transaction_processor.cpp`, suite `TransactionProcessorShutdown`)

| Test | Invariant | Failure mode caught |
|---|---|---|
| `StopDrainsSubmittedTransactions` | `start()` with 2 workers; submit 20; `stop()`; all 20 committed in repos/WAL | Dropped tail → committed count < 20 |
| `StopIdempotentAndDestructorSafe` | `stop(); stop();` then destruction | Double join / crash |
| `StopFromNonWorkerThreadWhileBusy` | Workers busy on slow-ish transactions (loop of deposits); external `stop()` completes within watchdog; no TSan report | Hang → watchdog; race → TSan |
| `WaitPopTokenUnblocksStarvedWorker` | `start()` with 1 worker, empty queue, submit nothing, `stop()` promptly returns | Worker stuck in un-interruptible wait → watchdog |

### H. Stress (`tests/test_stress.cpp`, suite `Stress`)

| Test | Invariant | Failure mode caught |
|---|---|---|
| `ConcurrentTransfersConserveMoney` | 32 accounts × 1000 cents; 4 pool workers; submit 500 random transfers (and some withdrawals); `stop()`; **sum of all balances == initial sum**; every balance ≥ 0; every successful submit has exactly one committed WAL record and one txn-repo record | Lost update → sum drifts; double-apply → drifts; WAL/state divergence → record-count mismatch |
| `DisjointAccountsWithoutLocksStillSafe` | 2 groups of accounts, transfers only within each group, 8 workers — checks throughput path and that no global serialization bug causes deadlock | Hang → watchdog; race → TSan |
| `RepeatedStartStopCycles` | 5 × (start, submit burst, stop) on one processor | State-machine bug in accepting_/closed flags → submit failures or leaked threads (TSan LSan) |

**Watchdog convention:** every potentially-blocking test wraps the blocking phase in `completes_within(fn, timeout)` (helper below); timeout → `FAIL() << "possible deadlock or missed wakeup"`. Timeouts generous (5 s) so slow CI doesn't false-positive, but finite so a deadlock fails instead of hanging the suite.

## Which tests must run under TSan

Everything that starts more than one thread, i.e. **all of** `test_thread_pool.cpp`, `test_transaction_queue.cpp` (the wait_pop/stop-token cases spawn a jthread), `test_account_locks.cpp`, `test_transaction_processor.cpp` (`start()`/`shutdown` cases), and `test_stress.cpp`. The pure single-threaded processor tests and the standalone `ScopedAccountLock` tests are TSan-neutral but harmless to run there. Since the `tsan` preset runs the whole `bank_tests` binary, the practical rule is: **the full suite must be TSan-clean**, and the stress suite is additionally tuned (smaller iteration counts if needed) so the tsan test preset finishes within a sane wall time. The `vm.mmap_rnd_bits=28` note applies to running the tsan preset (already handled in project setup).

## Test helpers to add to `tests/TestSupport.h`

(Existing `TempDir` stays as-is; everything below is additive, header-only, no new deps.)

```cpp
// Seeds `repo` with `count` accounts, each Active, owned by CustomerId{1},
// with `initial` balance. Returns the created AccountIds in order.
inline std::vector<bank::AccountId> seed_accounts(bank::InMemoryAccountRepository& repo,
                                                  std::size_t count,
                                                  bank::Money initial);

// Mutual-exclusion probe: enter() increments an atomic depth and records
// the max; assert max_depth() == 1 after a contention run.
class CriticalSectionProbe {
public:
    void enter() noexcept;          // ASSERT-style check done by max_depth()
    void leave() noexcept;
    [[nodiscard]] int max_depth() const noexcept;
private:
    std::atomic<int> depth_{0};
    std::atomic<int> max_depth_{0};
};

// Runs `fn` on a std::async and returns whether it finished within `limit`.
// On timeout: reports failure and does NOT join (test is already failed;
// std::future destructor of async blocks — document that timeout cases
// use std::launch::async + detach-free handling, or run fn on a jthread
// with a promise and accept the leak on timeout).
template <typename F>
[[nodiscard]] bool completes_within(F&& fn, std::chrono::milliseconds limit);

// Pops-and-collects helper: drains a TransactionQueue's try_pop into a
// vector (deterministic single-thread tests).
inline std::vector<bank::Transaction> drain_queue(bank::TransactionQueue& queue);

// Convenience: builds InMemoryAccountRepository + InMemoryTransactionRepository
// + InMemoryAuditRepository + WalLogger(TempDir) + TransactionProcessor{workers}
// in one struct, keeping TempDir alive. Used by processor & stress tests.
struct ProcessorHarness {
    explicit ProcessorHarness(std::size_t workers, std::size_t accounts,
                              bank::Money initial);
    bank::TempDir dir;                                 // must be declared first
    bank::InMemoryAccountRepository accounts;
    bank::InMemoryTransactionRepository transactions;
    bank::InMemoryAuditRepository audit;
    bank::WalLogger wal;
    bank::TransactionProcessor processor;
};
```

Includes added to `TestSupport.h`: `<atomic>`, `<chrono>`, `<future>`, `<latch>`, `<barrier>`, `"concurrency/TransactionProcessor.h"`, `"persistence/InMemory*.h"`. Note the header currently has no namespace; keep the new helpers in the global namespace alongside `TempDir` for consistency.
