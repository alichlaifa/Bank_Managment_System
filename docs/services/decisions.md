# 2. `docs/services/decisions.md`

## 1. Synchronous `process_one` for all money movements; `submit` is not exposed

`deposit`, `withdraw`, and `transfer` call `TransactionProcessor::process_one` and return the committed `TransactionRecord` or a failure. This is a request/response API: the caller needs to know whether the transaction committed, and if not, why.

`submit` is asynchronous. It returns only a reserved `TransactionId`; execution happens later on a worker, and failures are observable only through the audit log or by polling the repositories. That contract cannot satisfy `Result<TransactionRecord>` and would make the service's error handling dishonest. It would also make `balance()` and `history()` immediately racy with accepted-but-not-yet-executed work.

Therefore:

- Public mutations are synchronous and complete before returning.
- `submit` remains available on the processor for a future fire-and-forget, batching, or benchmark path.
- Switching a public method to `submit` would require returning a handle/future and reworking the read path; it is explicitly out of scope.

Freeze/unfreeze are synchronous direct mutations because there is no `Transaction` variant for account-status changes. They do not go through the WAL because they do not move money; they append an audit entry instead.

## 2. How `balance()` reads safely

`AccountRepository::find_by_id` is **not** thread-safe on its own:

- `InMemoryAccountRepository` is a plain `std::map` with no internal lock, by design.
- `SqliteAccountRepository` uses one `SqliteDatabase` connection; the class documents that the concurrency boundary lives above it.

`AccountLockManager` is not used by BankingService. `TransactionProcessor` owns a private `AccountLockManager locks_`; the milestone forbids changing the processor, and constructing a second manager in BankingService would protect different mutexes and provide no synchronization with processor workers.

The chosen lock is `BankingService::op_mutex_`, held for the duration of the `find_by_id` call and released immediately after copying the optional result. Every BankingService mutation also takes `op_mutex_` (around `process_one` or the freeze/unfreeze save), so service-issued writes cannot interleave with a service-issued balance read.

`op_mutex_` is an explicit correctness-over-throughput tradeoff, documented at the member:

> Serializes every public BankingService call, including reads and `process_one` mutations. Correctness over throughput: this milestone cannot share the processor's private `AccountLockManager`, so a service-owned mutex is the only way to keep `balance()`/`history()` from racing service-issued writes. Direct `processor.submit()` callers bypass this guarantee.

Consequence: BankingService serializes its own operations. If code calls `processor.submit()` directly and bypasses BankingService, reads are not protected. The composition root must treat BankingService as the only writer until the processor exposes a read path or a shareable lock/repo mutex. That future change is out of scope here.

## 3. How `history()` reads safely

`TransactionRepository::for_account` is also not thread-safe on its own: the InMemory implementation returns elements from an unprotected `std::vector`. SQLite prepares a statement per call, but it is still one connection with the concurrency boundary above it.

`history()` takes the same `BankingService::op_mutex_` for the duration of one `for_account` call. It does not need an account lock because it only reads append-only transaction records and does not inspect or mutate an `Account`. The service mutex is sufficient to serialize the read against service-issued transaction appends.

As with `balance()`, a direct asynchronous `submit()` that bypasses BankingService is outside this guarantee.

## 4. Session token generation, collision risk, and expiry

Tokens are 32 bytes from `std::random_device`, encoded as 64 lowercase hex characters. UUID was rejected because:

- C++20 has no standard UUID type.
- Adding a UUID library violates the no-new-dependencies constraint.
- 256 random bits give a larger space than a UUIDv4's 122 random bits.

Collision risk is `2^-256` for a single pair. `SessionManager::create` still checks for an existing token and regenerates on collision under the map mutex, so an accidental collision cannot replace a live session.

Expiry is lazy and checked on `validate()` (and `revoke()`): `expires_at` is set at creation, and a token whose `now >= expires_at` is erased and rejected. There is no background sweeper. This avoids a second thread lifecycle, keeps shutdown simple, and lets tests inject a fake clock. The cost is that expired entries can remain in the map until touched; acceptable for the current small session table.

`validate()` does not extend expiry. The default TTL is eight hours, marked in the header as **chosen for the demo**; it is constructor-injectable for tests and expected to become configurable policy later.

## 5. Why AuthService must not reveal whether a customer exists

A login endpoint that says "customer not found" is a customer-enumeration oracle. An attacker can probe IDs, learn which customers exist, and focus PIN guessing on real accounts. It also reveals business volume and account ownership patterns.

Both failure paths therefore produce:

- the same `Result` error string, `"invalid credentials"`;
- the same audit action, `"auth.login_failed"`;
- the same audit details, `"invalid credentials"`;
- the same claimed actor id;
- roughly the same amount of PBKDF2 work, because a missing customer is verified against a fixed dummy hash.

The audit log is allowed to record the claimed customer id for investigation, but it must not contain a field that distinguishes "missing" from "wrong PIN".

Audit writes are best-effort on both paths. On success, an audit failure must not deny a valid login: the session is returned normally and the audit error is not reclassified as an authentication failure. On failure, an audit error must not change the returned error string, because that would create a new oracle.

## 6. Login when a session already exists for that customer

Concurrent sessions are allowed. Every successful login creates a new independent token; earlier tokens remain valid until they expire or are explicitly revoked. Login does not revoke the customer's other sessions.

Reasons:

- A customer may legitimately be logged in on multiple devices.
- The service has no client/session identifier to decide which existing session is "the" one to replace.
- Revoking all sessions on every login would surprise other active clients and makes retry/reconnect behavior hostile.

Tradeoff: a stolen token remains valid until expiry or explicit logout. A future explicit "revoke all sessions for customer" or a per-customer session cap is deferred; it is not part of this milestone.

## 7. SessionManager thread safety

`SessionManager` owns one `std::unordered_map<std::string, Session>` and one `std::mutex` guarding it.

- `create`, `validate`, `revoke`, and `size` all lock the mutex.
- Token generation happens before insertion; collision checking and insertion happen under the lock.
- The map is read-mostly (validate is the common operation), but a plain `std::mutex` is used instead of `std::shared_mutex` because `validate()` may erase an expired session, which is a write. Avoiding a shared/exclusive upgrade path keeps the implementation simple.
- No lock is held across repository or audit calls; SessionManager is self-contained.
- Expired entries are removed lazily on access, as described above. There is no sweeper thread.

## 8. AuditService: wrapper plus filter helpers, not business logic

`AuditService` is a thin query wrapper over `AuditRepository`. It adds:

- `find_by_actor`,
- `find_by_action`,
- `find_since`.

Each helper constructs an `AuditFilter` and delegates to `find`. There is no caching, sorting beyond repository order, pagination, authorization, or business rule enforcement. RBAC belongs to the future API layer.

The class is read-only. Service-originated writes use `audit_detail::append` from the same header, which exists to solve two concrete problems:

1. InMemory audit repositories are not thread-safe; concurrent AuthService and BankingService writes need one shared mutex.
2. `AuditEntry.id` is a SQLite primary key. AuthService/BankingService cannot safely allocate from independent counters that may collide with each other or with the transaction processor's low IDs.

`audit_detail::append` uses a process-wide mutex and a shared atomic ID counter starting above the processor's range. Restart-safe seeding from the persisted maximum is a documented TODO.

## 9. Audit ID allocation for service writes

The transaction processor allocates audit IDs from `1` upward using its own atomic. Because modifying the processor is forbidden in this milestone, service writes cannot reuse that counter.

`audit_detail::append` therefore allocates from a reserved high base (`1'000'000'000'000'000`), which is far outside the processor's expected range and still inside SQLite's signed 64-bit integer range. This prevents primary-key collisions in the current process and persistence layout. The limitation is restart safety: after a process restart the counter must eventually be seeded from the persisted maximum; that is a TODO, not part of this milestone.

## 10. InterestService has no processor dependency in the stub

The stub takes only `AccountRepository&` and `AuditRepository&`. It does not take `TransactionProcessor&`, because it does not post any transactions yet. Adding an unused processor dependency would imply a contract the stub does not honor.

When `accrue_monthly_interest()` is implemented, it should take `TransactionProcessor&` back and post one interest `Deposit` per eligible account through the synchronous path. The scheduling decision (external caller vs. `std::jthread` + `stop_token`) remains a TODO; this milestone chooses no background thread.
