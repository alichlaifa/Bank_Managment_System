# 4. `docs/concurrency/build_order.md`

Each step is a vertical slice: add headers/sources + tests to CMake (the only CMake edits, done during implementation, listed here for reference only), build under `dev`, run the new tests, and only then proceed. Every step must leave the **full existing suite** green too.

CMake edits per step (for reference; not written in this plan pass):
- Steps 1–2 add header-only files plus `src/concurrency/AccountLockManager.cpp` + `src/domain/Types.h` hash specialization; test file `test_account_locks.cpp`.
- Steps 3–4 add their `.cpp` to `bank` and their test file to `bank_tests`.
- Step 5 adds `TransactionProcessor.cpp` + `test_transaction_processor.cpp`.
- Step 6 adds `test_stress.cpp`.

---

### Step 1 — `ScopedAccountLock` standalone (smallest compiling unit)

Files: `src/concurrency/ScopedAccountLock.h` (header-only; only the `std::mutex&` constructor, `AccountId` field, move ops, dtor — the `AccountLockManager&` ctor is **not** compiled yet; add it in step 2), `tests/test_account_locks.cpp` with only the `ScopedAccountLock` suite, `tests/TestSupport.h` gains `CriticalSectionProbe` and `completes_within`.

Acceptance check:
```
cmake --preset dev && cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=ScopedAccountLock.*
ctest --preset dev --output-on-failure
```
Expected: `ScopedAccountLock.*` tests PASS (≥3 tests), full existing suite still PASS, ASan/UBSan clean.

---

### Step 2 — `AccountLockManager` + `ScopedAccountLocks` + `std::hash` for `Id<Tag>`

Files: `src/domain/Types.h` (add hash partial specialization — the only domain edit), `src/concurrency/ScopedAccountLock.h` (add manager ctor), new `ScopedAccountLocks.h`, `AccountLockManager.h/.cpp`, extend `test_account_locks.cpp` with the `AccountLockManager` suite (single-account, two-account sorted, same-id, mutex_count, distinct-accounts-don't-block), extend `TestSupport.h` with `seed_accounts`.

Acceptance check:
```
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=ScopedAccountLock*:AccountLockManager*
cmake --preset tsan && cmake --build --preset tsan --target bank_tests
ctest --preset tsan --output-on-failure
```
Expected: all lock tests PASS under both; TSan reports zero races on the contention tests; the two-account ABBA loop (1'000 barrier-released iterations) completes in well under the 5 s watchdog.

---

### Step 3 — `TransactionQueue`

Files: `src/concurrency/TransactionQueue.h/.cpp`, `tests/test_transaction_queue.cpp` (FIFO, close semantics, wait_pop + stop_token with jthread/latch), `TestSupport.h` gains `drain_queue`.

Acceptance check:
```
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=TransactionQueue.*
ctest --preset dev --output-on-failure
```
Expected: all `TransactionQueue.*` PASS; the stop-token wakeup test completes < 1 s (no hang); full suite green under `dev`.

---

### Step 4 — `ThreadPool`

Files: `src/concurrency/ThreadPool.h/.cpp`, `tests/test_thread_pool.cpp` (tasks-execute, pending, submit-after-stop, idempotent stop, drain-on-stop, 0-thread pool, many-threads-many-tasks).

Acceptance check:
```
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=ThreadPool.*
cmake --build --preset tsan --target bank_tests && ctest --preset tsan --output-on-failure
```
Expected: all `ThreadPool.*` PASS under dev and the whole suite TSan-clean; `ManyThreadsManyTasks` count == 1000.

---

### Step 5 — `TransactionProcessor` + single-threaded tests

Files: `src/concurrency/TransactionProcessor.h/.cpp`, `tests/test_transaction_processor.cpp` (synchronous suite via `worker_count == 0` + `process_one`/`process_until_empty`, plus the shutdown suite using `start()`/`stop()`), `TestSupport.h` gains `ProcessorHarness`. Note in WalLogger.h: replace the "single-threaded by contract" comment and add the `mutable std::mutex mutex_` member + lock in the five public methods (implementation lives in the existing `WalLogger.cpp`; existing `test_wal.cpp` must stay green — it is single-threaded and unaffected).

Acceptance check:
```
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=TransactionProcessor*:TransactionProcessorShutdown*:Wal.*
cmake --preset tsan && cmake --build --preset tsan --target bank_tests && ctest --preset tsan --output-on-failure
```
Expected: all processor tests PASS; `Wal.*` regression suite still green (WalLogger mutex didn't break single-threaded behavior); TSan-clean shutdown tests; `WalFailureInsideLockLeavesStateIntact` fails with `"wal append failed"` and unchanged balances.

---

### Step 6 — Multi-threaded stress

Files: `tests/test_stress.cpp` (concurrent transfers conserve money, disjoint-account safety, repeated start/stop cycles; barrier-seeded ABBA seeded into the account-locks stress case). No new src files.

Acceptance check:
```
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter=Stress.*
./build/dev/bank_tests          # entire suite, ASan+UBSan
cmake --build --preset release --target bank_tests && ctest --preset release --output-on-failure
cmake --preset tsan && cmake --build --preset tsan --target bank_tests && ctest --preset tsan --output-on-failure
ctest --preset dev --output-on-failure
```
Expected: all `Stress.*` PASS; conservation-of-money assertion holds; every preset (`dev`, `release`, `tsan`) green; no watchdog trips (no deadlocks), no sanitizer reports. This is the milestone exit criterion.

---

**Out of scope (explicitly deferred):** optimistic version-checking on `AccountRepository::save` (the `version_` TODO), bounded queues, priorities, metrics, `std::expected` migration of `Result`, any UML updates.
