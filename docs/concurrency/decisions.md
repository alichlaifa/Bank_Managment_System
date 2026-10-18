# 2. `docs/concurrency/decisions.md`

## 1. `std::mutex` per account, not `std::shared_mutex`

Every operation the processor performs under an account lock is a **write**: `Account::deposit`/`withdraw` mutate balance and bump `version_`; `AccountRepository::save` overwrites the row. There is no read-only critical section in the pipeline — `txn::validate` reads, but it is always followed by `txn::apply` in the same critical section, and holding the lock across validate+apply is required for correctness (otherwise a validated snapshot is stale by apply time). A `shared_mutex` would therefore put every acquisition on the exclusive path anyway, while costing ~2× the memory and more uncontended-lock overhead than `std::mutex`, and it invites a future contributor to take a "read" lock over `validate` and silently introduce a check-then-act race. The lock type is an implementation detail behind `AccountLockManager`; switching later if a genuine read path appears is a one-file change.

## 2. `AccountLockManager` owns the mutexes, not `Account`

`Account` is a value type: it is copied by value out of `AccountRepository::find_by_id()` on every load (the repo returns `std::optional<Account>`, not a reference). A mutex inside `Account` would be (a) copied — `std::mutex` is non-copyable, forcing a custom copy ctor that drops or fakes the lock, or a `shared_ptr<mutex>` with its own lifetime hazards; (b) relocated on every vector/store operation, invalidating any thread holding a lock "on" the old copy; (c) serialized by `Serializer`/`account_to_json`, mixing concurrency state into the persistence schema. Keeping the mutex table in a sidecar keyed by `AccountId` means lock identity is tied to the *identity* of the account, not to any particular copy of its state, and `Account` stays a pure value object. The tradeoff — lock table lookups on every acquire — is O(1) hash access guarded by one `map_mutex_`, negligible next to a WAL append.

## 3. WAL append happens inside the lock

Pipeline: `validate → WAL intent → apply → WAL commit → unlock`. If the WAL append were done after unlocking, two interleavings break atomicity:

- **Lost intent:** thread A applies `withdraw(100)` from account X and releases the lock; then crashes *before* its WAL append. The in-memory/SQLite state says balance 900, the journal says nothing. Recovery replays the WAL, sees no record, and resurrects balance 1000. Divergence between journal and store, undetectable.
- **Out-of-order journal:** A and B both complete apply for overlapping accounts; their WAL appends race, so the journal can order B's intent before A's even though A's effect is already visible. Replay applies updates in the wrong order → wrong final balance.

Holding the lock across the whole sequence makes "the state a reader can observe" and "the order of WAL records" the same order, which is exactly the invariant recovery assumes. Cost: the lock is held across a file append — acceptable, because correctness > throughput here, the append is one buffered write + in-place byte flip, and this is the design the milestone's recovery story (snapshot + replay-from-lsn) depends on.

## 4. `WalLogger` needs an internal mutex now

`WalLogger`'s header says append semantics are "single-threaded by contract: the transaction processor funnels writes through its own locking." Once the thread pool exists, that contract is false on two counts: (a) `TransactionProcessor::process_one()` runs on multiple pool workers simultaneously; (b) each worker only holds *account* locks, and two transactions touching **disjoint** accounts never contend — both can reach `wal_.append`/`mark_committed` concurrently. `WalLogger`'s mutable state (`entries_`, `next_offset_`, in-place header rewrite in `mark_committed`) would race, and the file itself would interleave partial lines. The fix is one `mutable std::mutex mutex_` held by `append`, `mark_committed`, `size`, `load_all`, and `replay_from`. This mutex is *inside* the account-lock critical section, so the lock order is: account mutex(es) → WalLogger mutex → (repo mutexes if any) — one global order, no cycle (see §5).

## 5. Sorted-id locking avoids deadlock — proof sketch

**Protocol.** Every code path that locks a set of account mutexes acquires them strictly in ascending `AccountId` order (`ScopedAccountLocks` sorts its two ids; a same-id pair acquires one mutex). Assume for contradiction a deadlock exists. Deadlock implies a **wait-for cycle**: threads T₁ → T₂ → … → Tₙ → T₁, where Tᵢ is blocked waiting for a mutex held by Tᵢ₊₁ (indices mod n).

For each blocked Tᵢ, define `w(Tᵢ)` = the id of the mutex it is currently waiting for. Tᵢ holds a set `H(Tᵢ)` of mutexes, all already acquired. By the protocol, every acquisition by Tᵢ was in ascending order, so **every id in `H(Tᵢ)` is strictly less than `w(Tᵢ)`**: Tᵢ acquires its lowest-id lock first and can only ever wait for a not-yet-acquired, hence higher, id.

Now pick the cycle member Tₖ whose waiting id `w(Tₖ)` is **minimal** among all `w(Tᵢ)` in the cycle. Tₖ waits for `w(Tₖ)`, which is held by Tₖ₊₁ (that's what the wait-for edge means). So `w(Tₖ) ∈ H(Tₖ₊₁)`. By the protocol invariant applied to Tₖ₊₁: `w(Tₖ) < w(Tₖ₊₁)` — because every held id is strictly less than the id that holder is waiting for. Thus `w(Tₖ₊₁) > w(Tₖ)`, contradicting the minimality of `w(Tₖ)` among cycle members. (The same argument chains around: ids strictly increase along the cycle, so returning to Tₖ would require `w(Tₖ) > w(Tₖ)`, absurd.) ∎

Two corollaries that the tests will check:
- The proof only needs *every* multi-lock site to sort; single-lock sites are trivially ordered.
- The WalLogger/repo mutexes must sit *below* account locks in a global order (acquired only while account locks are held, never the reverse). No code path acquires an account lock while holding the WalLogger mutex, so no cycle can involve them.

## 6. Shutdown semantics (exact contract)

`TransactionProcessor::stop()` / `ThreadPool::stop()`:

| Property | Contract |
|---|---|
| **Drain or refuse?** | **Both, in order.** `stop()` *refuses new work first* (`accepting_ = false`, then `queue_.close()` / `stopping_ = true`, so subsequent `submit()` returns `false` / `Result::failure("processor stopped")`), then *drains what was already accepted*: queued transactions/tasks run to completion before their worker exits. Nothing accepted is silently dropped. |
| **Idempotent?** | Yes. A second `stop()` on either class is a no-op (guarded by the mutex-protected flag; joins on an already-joined `jthread` vector are skipped by clearing it after join). `~TransactionProcessor`/`~ThreadPool` call `stop()`, so destruction after an explicit `stop()` is safe. |
| **Callable from any thread?** | `stop()` may be called from any thread **except** a worker of the same pool (it would join itself → self-deadlock). This is a documented precondition, checked with an assert in debug builds. Calling `stop()` concurrently from two external threads is safe (mutex serializes; one performs the join, the other observes "already stopped"). |
| **How workers drain** | `ThreadPool`: worker predicate is `!tasks_.empty() || stopping_`; when `stopping_ && empty` the worker returns, so every task submitted before `stop()` runs. `TransactionProcessor`: `stop()` calls `shutdown_.request_stop()` **and** `queue_.close()`. Consumer loop: `while (auto txn = queue_.wait_pop(shutdown_.get_token())) process_one_impl(...)`. `close()` + notify wakes every waiter; each consumer pops and processes until the queue is empty, then `wait_pop` returns `nullopt` and the consumer task returns — after which `pool_.stop()` joins the consumer tasks. |
| **`condition_variable_any::wait(stop_token)` usage** | All blocking waits are `cv_.wait(lock, token, pred)` (the three-argument stop-token overload). If `token` is stopped *and* `pred` is false, the wait returns immediately; the consumer then re-checks `closed_ && empty` and exits. This means a `wait_pop` blocked on an empty, still-open queue can still be released by `request_stop()` alone — the belt to `close()`'s braces — and guarantees no worker can hang in `stop()` if a code path forgets a `notify`. `ThreadPool::worker` uses its `std::jthread`'s built-in token the same way. |

## 7. `Result<T>` / `Result<void>` in the public API

- `submit()` returns `Result<TransactionId>`: success carries the assigned id; failure strings are `"processor stopped"` or, if the queue refuses for any other reason, `"queue closed"`.
- `process_one()` returns `Result<TransactionRecord>`: success carries the final record with `state == TransactionState::Committed`; failures carry non-empty strings: `"account not found: <id>"`, `"validation failed: insufficient funds"`, `"validation failed: account frozen"`, `"validation failed: same account transfer"`, `"wal append failed: <what>"`, `"wal commit failed: <what>"`.
- `process_until_empty()` returns `std::vector<Result<TransactionRecord>>` (one entry per transaction, FIFO) rather than `Result<...>` so a single bad transaction doesn't mask the outcomes of the rest.
- The internal `process_impl` catches `std::exception` from `Account` mutators, repos, and `WalLogger` and converts to `Result::failure` — the processor never throws across its public API. **Trap honored:** `Result<void>::failure("")` reports `ok()`, so every failure string is asserted non-empty in tests.
- No monadic chaining is used (Result has no `and_then`/`map`); each call site checks `ok()`/`failed()` explicitly, matching Result's current design.

## 8. WAL append failure inside the lock

Two sub-cases, both handled inside the account-lock critical section, both leaving the lock released by RAII on the way out:

1. **`wal.append(intent)` throws** (disk full, permissions): nothing has been applied. `process_impl` catches, appends an `AuditEntry{action: "txn.failed", details: "wal append failed: ..."}`, returns `Result::failure("wal append failed: ...")`. The account state is untouched; a partial line already on disk is the existing "interrupted tail" case that `WalLogger`'s constructor truncates on next open. Consistent.
2. **`wal.mark_committed(id)` throws after apply** (the worst case): accounts have been mutated and saved, but the journal line still says `Validated`. Contract: `process_impl` returns `Result::failure("wal commit failed: ...")` and writes the audit entry; the transaction is treated as **not committed** — recovery (snapshot + `replay_from`) ignores a `Validated`-but-not-`Committed` line, so on restart the persisted accounts reconcile to the committed-only view. In the running process the in-memory/SQLite balances may transiently disagree with the journal until recovery; this is accepted and documented because the only realistic throw source is an I/O error that is about to take the process down anyway. We do **not** attempt in-process rollback of the applied mutation: `Account` has no inverse-operation API, and inventing one (compensating transfer) is out of scope for this milestone.
