# 1. `docs/services/interfaces.md`

## Conventions

- All service types live in `namespace bank`, in `src/services/`.
- Headers are `#pragma once`; project includes are root-relative from `src/`.
- Public failures use `Result<T>` / `Result<void>`. Every failure string is non-empty (important because `Result<void>::ok()` is defined by an empty error string).
- Domain/value-object exceptions are caught at the service boundary and translated to `Result::failure`. Services do not throw across their public API.
- Repositories remain non-owning, injected references. Services never own storage.
- Service-originated audit writes go through `audit_detail::append` (declared in `services/AuditService.h`, implemented in `AuditService.cpp`). It serializes appends with one process-wide mutex and allocates IDs from a shared atomic. This avoids both InMemory audit races and SQLite `audit_log.id` primary-key collisions with the transaction processor's low-numbered audit IDs.
- Audit writes are best-effort on successful login: an audit failure does not turn a valid credential check into a failed login.

## `services/SessionManager.h`

### Includes

```cpp
#pragma once

#include "core/TimeUtil.h"
#include "domain/Session.h"
#include "domain/Types.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
```

### Public interface

```cpp
class SessionManager {
public:
    using Clock = timeutil::Clock;
    using NowFn = std::function<Clock::time_point()>;

    explicit SessionManager(
        // 8 hours: chosen for the demo; production policy should be
        // configurable and likely shorter, with refresh/rotation.
        std::chrono::seconds ttl = std::chrono::hours{8},
        NowFn now = &timeutil::now);

    [[nodiscard]] Session create(CustomerId customer_id, Role role);

    [[nodiscard]] std::optional<Session> validate(std::string_view token);

    // Atomically removes a live token and returns the removed session.
    // Returns nullopt for an unknown/expired token.
    [[nodiscard]] std::optional<Session> revoke(std::string_view token);

    // Diagnostic/test helper: number of entries currently stored.
    // Expired entries are removed lazily by validate()/revoke().
    [[nodiscard]] std::size_t size() const;

private:
    [[nodiscard]] static std::string generate_token();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
    std::chrono::seconds ttl_;
    NowFn now_;
};
```

### Dependencies

| Component | Relationship | Injection |
|---|---|---|
| `timeutil::Clock` / `timeutil::now` | value / function | `NowFn` constructor parameter, defaults to `&timeutil::now` |
| Session state | owned by value in `std::unordered_map<std::string, Session>` | internal |
| `std::mutex` | owned; guards the map | internal |

No repositories, no persistence. SessionManager is process-local.

### Token generation, expiry, sweeping

- Tokens are generated from `std::random_device`, not UUID. 32 random bytes are hex-encoded to 64 lowercase hex characters.
- No UUID library is added; the standard library has no UUID type, and random bytes give 256 bits versus a UUIDv4's 122 bits.
- Collision risk is nominal `2^-256`. `create()` still checks the map and regenerates on collision while holding the mutex.
- Expiry is **lazy**: `create()` sets `expires_at = now_() + ttl_`; `validate()` checks `now_() >= expires_at` and erases the entry if expired.
- There is **no background sweeper**. Expired entries remain in the map until a later `validate()`, `revoke()`, or diagnostic `size()` observation. This is deliberate for this milestone: no extra thread, no shutdown contract, deterministic tests via injected `NowFn`.
- `validate()` does not extend expiry.
- A newly created session has `state == SessionState::Authenticated`.
- `revoke()` erases the entry and returns a copy with `state == SessionState::Revoked`.
- The default TTL of eight hours is **chosen for the demo**; it is constructor-injectable for tests and is expected to become configurable policy later.

## `services/AuthService.h`

### Includes

```cpp
#pragma once

#include "core/Result.h"
#include "domain/Customer.h"
#include "domain/Session.h"
#include "domain/Types.h"
#include "persistence/AuditRepository.h"
#include "persistence/CustomerRepository.h"
#include "services/SessionManager.h"

#include <string>
#include <string_view>
```

`AuthService.cpp` additionally includes `services/AuditService.h` for `audit_detail::append` and `domain/PasswordHasher.h`.

### Public interface

```cpp
class AuthService {
public:
    AuthService(CustomerRepository& customers,
                SessionManager& sessions,
                AuditRepository& audit);

    [[nodiscard]] Result<Session> login(CustomerId customer_id,
                                        std::string_view pin);

    [[nodiscard]] Result<void> logout(std::string_view token);

private:
    [[nodiscard]] Result<void> audit(CustomerId actor,
                                     std::string action,
                                     std::string details);

    CustomerRepository& customers_;
    SessionManager& sessions_;
    AuditRepository& audit_;
};
```

### Dependencies

| Component | Relationship | Injection |
|---|---|---|
| `CustomerRepository` | non-owning reference; `find_by_id` only | constructor |
| `SessionManager` | non-owning reference; `create` / `revoke` | constructor |
| `AuditRepository` | non-owning reference; append failures/success/logout | constructor |
| `PasswordHasher` | static utility, no instance | used inside `login` for PIN verification and dummy verification |
| `audit_detail::append` | free helper | used from `.cpp` |

### Failure auditing without existence leakage

`login()` follows one path for all failures:

1. `customers_.find_by_id(customer_id)`.
2. If missing, verify the supplied PIN against a fixed dummy `PasswordHash` using `PasswordHasher::verify`. This performs the same PBKDF2 work as the existing-customer path and reduces timing differences.
3. If present, verify against the stored hash.
4. On either failure, append exactly one audit entry:
   - `action = "auth.login_failed"`
   - `actor_id = customer_id` (the claimed id, in both cases)
   - `details = "invalid credentials"` (identical in both cases)
5. Return the identical failure string `"invalid credentials"` for both cases.

No error text, audit action, audit detail, or return value distinguishes a nonexistent customer from an existing customer with a wrong PIN. A successful login appends `"auth.login"` with the authenticated customer id and creates one session.

Audit is **best-effort on the success path**: if `audit_detail::append` fails, `login()` still returns the created `Session`. A broken audit sink must not deny a valid customer access. The failure is not silently reclassified as an authentication failure, and the session is not revoked. Audit on failure is also best-effort; the return value remains `"invalid credentials"` so an audit error cannot become an existence oracle.

`logout()` calls `SessionManager::revoke(token)`. On success it appends `"auth.logout"` with the revoked session's customer id. An unknown/expired/revoked token returns `Result<void>::failure("invalid session")` and creates no audit entry.

## `services/BankingService.h`

### Includes

```cpp
#pragma once

#include "concurrency/TransactionProcessor.h"
#include "core/Result.h"
#include "domain/Account.h"
#include "domain/Money.h"
#include "domain/Transaction.h"
#include "domain/Types.h"
#include "persistence/AccountRepository.h"
#include "persistence/AuditRepository.h"
#include "persistence/TransactionRepository.h"

#include <mutex>
#include <vector>
```

`BankingService.cpp` additionally includes `services/AuditService.h` for `audit_detail::append` and `domain/TransactionOps.h` where needed.

### Public interface

```cpp
class BankingService {
public:
    BankingService(TransactionProcessor& processor,
                   AccountRepository& accounts,
                   TransactionRepository& transactions,
                   AuditRepository& audit);

    [[nodiscard]] Result<TransactionRecord> deposit(AccountId account_id,
                                                    Money amount);

    [[nodiscard]] Result<TransactionRecord> withdraw(AccountId account_id,
                                                     Money amount);

    [[nodiscard]] Result<TransactionRecord> transfer(AccountId from,
                                                     AccountId to,
                                                     Money amount);

    [[nodiscard]] Result<Money> balance(AccountId account_id) const;

    [[nodiscard]] Result<std::vector<TransactionRecord>>
    history(AccountId account_id) const;

    [[nodiscard]] Result<void> freeze(AccountId account_id);

    [[nodiscard]] Result<void> unfreeze(AccountId account_id);

private:
    TransactionProcessor& processor_;
    AccountRepository& accounts_;
    TransactionRepository& transactions_;
    AuditRepository& audit_;

    // Serializes every public BankingService call, including reads and
    // process_one mutations. Correctness over throughput: this milestone
    // cannot share the processor's private AccountLockManager, so a
    // service-owned mutex is the only way to keep balance()/history() from
    // racing service-issued writes. Direct processor.submit() callers bypass
    // this guarantee.
    mutable std::mutex op_mutex_;
};
```

### Dependencies

| Component | Relationship | Injection |
|---|---|---|
| `TransactionProcessor` | non-owning reference; synchronous transaction execution | constructor |
| `AccountRepository` | non-owning reference; reads for `balance`, direct status writes for freeze/unfreeze | constructor |
| `TransactionRepository` | non-owning reference; `history` reads | constructor |
| `AuditRepository` | non-owning reference; freeze/unfreeze audit entries | constructor |
| `AccountLockManager` | **not injected** in this milestone; the processor owns its own private instance | — |
| `std::mutex op_mutex_` | owned; serializes every public BankingService call | internal |

`AccountLockManager` is intentionally absent from the constructor. `TransactionProcessor` owns its lock manager by value, and the milestone forbids modifying the processor. A second, service-owned lock manager would not synchronize with the processor's workers. The service-level `op_mutex_` is the coordination point for this milestone; see `decisions.md`.

### Delegation map for every public method

| Method | Return | Execution path | Processor call |
|---|---|---|---|
| `deposit` | `Result<TransactionRecord>` | synchronous transaction | `process_one(Deposit{...})` |
| `withdraw` | `Result<TransactionRecord>` | synchronous transaction | `process_one(Withdraw{...})` |
| `transfer` | `Result<TransactionRecord>` | synchronous transaction | `process_one(Transfer{...})` |
| `balance` | `Result<Money>` | pure read | none |
| `history` | `Result<std::vector<TransactionRecord>>` | pure read | none |
| `freeze` | `Result<void>` | synchronous direct account metadata mutation + audit | none; there is no `Transaction` variant for status changes |
| `unfreeze` | `Result<void>` | synchronous direct account metadata mutation + audit | none; same reason |

- No public BankingService method uses `TransactionProcessor::submit`. `submit` is asynchronous and returns only a reserved `TransactionId`; the service API needs a committed `TransactionRecord` or a concrete error.
- `freeze` sets an existing account to `AccountStatus::Frozen`. Freezing an already-frozen account is an idempotent success with no audit duplicate. A missing account fails; a `Closed` account is not silently reopened.
- `unfreeze` sets a frozen account to `Active`. Unfreezing an active account is an idempotent success. A missing account fails; a `Closed` account fails.
- `balance` returns `"account not found: <id>"` for a missing account.
- `history` returns the transaction repository's records for the account; an unknown account yields a successful empty vector rather than a fabricated error. It does not need account existence for a repository read.
- Deposit/withdraw/transfer do not append their own audit entries: `TransactionProcessor` already appends `"txn.completed"` / `"txn.failed"`.

## `services/InterestService.h`

### Includes

```cpp
#pragma once

#include "core/Result.h"
#include "persistence/AccountRepository.h"
#include "persistence/AuditRepository.h"
```

### Public interface

```cpp
class InterestService {
public:
    InterestService(AccountRepository& accounts,
                    AuditRepository& audit);

    [[nodiscard]] Result<void> accrue_monthly_interest();

private:
    AccountRepository& accounts_;
    AuditRepository& audit_;
};
```

### Dependencies and scheduling

| Component | Relationship | Injection |
|---|---|---|
| `AccountRepository` | non-owning reference; future account enumeration | constructor |
| `AuditRepository` | non-owning reference; future `interest.accrued` audit entries | constructor |

This milestone ships a compiling stub only. `accrue_monthly_interest()` currently returns `Result<void>::success()` without changing balances or writing audit entries.

It does **not** take a `TransactionProcessor&` in this milestone. The processor dependency is added back when `accrue_monthly_interest()` is implemented and actually posts interest deposits.

It does **not** own a `std::jthread` and does **not** use `std::stop_token` in this milestone. Scheduling is deferred:

```cpp
// TODO(interest-scheduling): decide whether the background scheduler owns a
// std::jthread with stop_token, or whether an external scheduler calls
// accrue_monthly_interest() periodically. Do not add a thread until the
// scheduler milestone defines startup/shutdown ordering and test control.
```

The external-scheduler option is the default direction because it avoids a second lifecycle/stop contract and makes tests deterministic.

## `services/AuditService.h`

### Includes

```cpp
#pragma once

#include "core/Result.h"
#include "core/TimeUtil.h"
#include "domain/AuditEntry.h"
#include "domain/Types.h"
#include "persistence/AuditFilter.h"
#include "persistence/AuditRepository.h"

#include <string>
#include <vector>
```

### Public interface

```cpp
namespace audit_detail {

// Serializes all service-originated audit writes and allocates IDs from one
// process-wide counter. Defined in AuditService.cpp.
[[nodiscard]] Result<void> append(AuditRepository& audit,
                                  CustomerId actor,
                                  std::string action,
                                  std::string details);

} // namespace audit_detail

class AuditService {
public:
    explicit AuditService(AuditRepository& audit);

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find(const AuditFilter& filter) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_by_actor(CustomerId actor) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_by_action(std::string action) const;

    [[nodiscard]] Result<std::vector<AuditEntry>>
    find_since(timeutil::Clock::time_point since) const;

private:
    AuditRepository& audit_;
};
```

### Dependencies

| Component | Relationship | Injection |
|---|---|---|
| `AuditRepository` | non-owning reference; all queries delegate to `find` | constructor |
| `AuditFilter` | value; convenience methods construct filters | — |

`AuditService` is a thin query facade. It adds three convenience filter builders (`find_by_actor`, `find_by_action`, `find_since`) but no caching, no authorization, and no business logic. RBAC belongs to the future `BankServer`. Repository exceptions are caught and returned as `Result::failure("audit query failed: ...")`.

`audit_detail::append` is the shared write path for AuthService and BankingService. It stamps the timestamp, allocates an ID, serializes with a single mutex, and converts repository exceptions to `Result<void>::failure`. Audit IDs start at a high reserved base (`1'000'000'000'000'000`) so service IDs cannot collide with the transaction processor's low IDs. Seeding that base from a persisted maximum on restart is a TODO for the persistence/restart milestone.
