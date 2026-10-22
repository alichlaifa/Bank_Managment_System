# 3. `docs/services/tests.md`

All tests are plain GTest in the four deliverable test files. Existing style is preserved: `using namespace bank;`, anonymous-namespace helpers, assertions on `Result::ok()` / `error()`.

## Test helpers to add to `tests/TestSupport.h`

Additive only, header-only, no new dependencies.

```cpp
// Deterministic PIN fixture. Reuses one PasswordHasher derivation per PIN
// so service tests do not pay PBKDF2 cost repeatedly.
inline bank::PasswordHash pin_hash_for(std::string_view pin);

inline bank::Customer make_customer(bank::CustomerId id,
                                    std::string_view pin,
                                    bank::Role role);

inline bank::Account make_account(bank::AccountId id,
                                  bank::CustomerId owner,
                                  bank::Money balance,
                                  bank::AccountStatus status = bank::AccountStatus::Active);

// Seeds one customer into a CustomerRepository.
inline void seed_customer(bank::InMemoryCustomerRepository& repo,
                          bank::CustomerId id,
                          std::string_view pin = "1234",
                          bank::Role role = bank::Role::Customer);

// Seeds one account into an AccountRepository and returns its id.
inline bank::AccountId seed_account(bank::InMemoryAccountRepository& repo,
                                    bank::AccountId id,
                                    bank::CustomerId owner,
                                    bank::Money initial,
                                    bank::AccountStatus status = bank::AccountStatus::Active);

// Fake clock for SessionManager expiry tests.
struct FakeClock {
    bank::timeutil::Clock::time_point current;
    bank::timeutil::Clock::time_point now() const noexcept;
    void advance(std::chrono::seconds delta);
};

// Audit repository that fails every append. Used to prove login success does
// not depend on audit availability.
struct ThrowingAuditRepository : bank::AuditRepository {
    void append(const bank::AuditEntry&) override
    {
        throw std::runtime_error{"audit sink unavailable"};
    }

    std::vector<bank::AuditEntry> find(const bank::AuditFilter&) const override
    {
        return {};
    }
};

// Full service harness. Declaration order is construction order.
struct ServiceHarness {
    explicit ServiceHarness(std::size_t accounts = 0,
                            bank::Money initial = bank::Money{0});

    bank::TempDir dir;                                  // must be first
    bank::InMemoryCustomerRepository customers;
    bank::InMemoryAccountRepository accounts;
    bank::InMemoryTransactionRepository transactions;
    bank::InMemoryAuditRepository audit;
    bank::WalLogger wal;
    bank::TransactionProcessor processor;               // worker_count == 0
    bank::SessionManager sessions;
    bank::AuthService auth;
    bank::BankingService banking;
    bank::AuditService audit_query;
    bank::InterestService interest;
};

// Money and audit helpers.
inline bank::Money total_balance(const bank::AccountRepository& accounts);
inline std::vector<std::string> audit_actions(const bank::AuditRepository& audit);
inline bool has_audit(const bank::AuditRepository& audit,
                      std::string_view action);
inline std::size_t transaction_count(const bank::TransactionRepository& txns);

// Concurrency helper: spawn `threads`, each creating `per_thread` tokens from
// one SessionManager, release them together with a std::barrier, and return
// every token. Used by the 4 x 100 uniqueness test.
inline std::vector<std::string>
create_tokens_concurrently(bank::SessionManager& sessions,
                           bank::CustomerId customer,
                           int threads,
                           int per_thread);
```

Additional includes in `TestSupport.h`: `<barrier>`, `<latch>`, `<set>`, `<stdexcept>`, `"concurrency/TransactionProcessor.h"`, `"persistence/InMemoryCustomerRepository.h"`, `"persistence/InMemoryTransactionRepository.h"`, `"persistence/InMemoryAuditRepository.h"`, `"services/AuthService.h"`, `"services/BankingService.h"`, `"services/SessionManager.h"`, `"services/AuditService.h"`, `"services/InterestService.h"`.

`ServiceHarness` deliberately constructs `TransactionProcessor` with `worker_count == 0`. Service methods use `process_one`/reads only, so no background workers are needed and tests remain deterministic. A concurrent test may call BankingService from multiple threads directly. `InterestService` is constructed with only the account and audit repositories; the processor dependency is added when interest accrual is implemented.

## Suite `SessionManager` — `tests/test_session_manager.cpp`

| Test | Invariant |
|---|---|
| `CreateProducesAuthenticatedSession` | Returned session has non-empty token, correct customer/role, `state == Authenticated`, `expires_at > now` |
| `CreateUsesInjectedTtl` | With `ttl = 10s` and fake clock, `expires_at == start + 10s` |
| `ValidateReturnsLiveSession` | `validate(token)` returns the same customer/role/expiry |
| `ValidateUnknownTokenReturnsNullopt` | Random string → `nullopt` |
| `ValidateExpiredTokenReturnsNulloptAndPurges` | Advance fake clock past expiry; validate → `nullopt`; `size()` drops |
| `ValidateDoesNotExtendExpiry` | Two validates at different times leave expiry unchanged |
| `RevokeInvalidatesToken` | `revoke(token)` returns the session; subsequent `validate` → `nullopt` |
| `RevokeUnknownTokenReturnsNullopt` | Unknown token → `nullopt`, no crash |
| `RevokeIsIdempotent` | Second revoke → `nullopt`; map size unchanged |
| `TokensAre64LowercaseHex` | Length 64; every character in `[0-9a-f]` |
| `SequentialTokensAreUnique` | 1,000 creates → 1,000 distinct strings; `size() == 1000` |
| `ConcurrentTokenCreationIsUnique` | 4 threads × 100 creates, barrier-released: 400 tokens, all unique, `size() == 400` |
| `ConcurrentValidateAndRevokeDoNotRace` | Threads validate while one revokes; every result is either the live session or `nullopt`, never a crash/race |

## Suite `AuthService` — `tests/test_auth_service.cpp`

| Test | Invariant |
|---|---|
| `LoginWithCorrectPinReturnsSession` | `ok()`, session customer/role correct, token validates through SessionManager |
| `LoginSuccessIsAudited` | Exactly one `"auth.login"` entry with the customer as actor |
| `LoginWrongPinForExistingCustomerFails` | `failed()`, error is non-empty and equals `"invalid credentials"`, no session created |
| `LoginWrongPinIsAudited` | Exactly one `"auth.login_failed"` entry; no success entry |
| `LoginNonexistentCustomerFails` | `failed()`, error is non-empty and equals `"invalid credentials"` |
| `LoginNonexistentCustomerIsAudited` | Exactly one `"auth.login_failed"` entry; same action/details as the wrong-PIN case |
| `BothLoginFailurePathsProduceTheSameErrorMessage` | Capture errors from existing-customer-wrong-PIN and nonexistent-customer; assert byte-for-byte equality |
| `FailedLoginDoesNotCreateSession` | SessionManager size stays 0; no token can be validated |
| `LoginSucceedsEvenIfAuditFails` | Inject a `ThrowingAuditRepository`; correct PIN still returns `ok()` with a valid session. Audit is best-effort on the success path: an audit failure never turns a valid login into a failure |
| `LogoutRevokesSession` | `logout(token).ok()`; subsequent `validate(token)` → `nullopt` |
| `LoggedOutSessionIsInvalid` | Login, logout, then validate/revoke both return `nullopt` |
| `LogoutUnknownTokenFails` | `failed()` with `"invalid session"` and a non-empty error |
| `LogoutIsAudited` | One `"auth.logout"` entry with the authenticated customer id |
| `SuccessfulLoginThenFailedLoginAuditCountsAreIndependent` | One success entry and one failure entry; neither suppresses the other |

## Suite `BankingService` — `tests/test_banking_service.cpp`

| Test | Invariant |
|---|---|
| `DepositReturnsCommittedRecord` | `ok()`; record state `Committed`; type `Deposit`; returned amount matches |
| `DepositUpdatesBalanceAndHistory` | Balance increases exactly; one transaction record appears |
| `DepositMissingAccountFails` | `failed()`, error contains `"account not found"`, balance unchanged, no transaction record |
| `WithdrawReturnsCommittedRecord` | `ok()`; state `Committed`; balance decreases exactly |
| `WithdrawInsufficientFundsFails` | `failed()`, non-empty error, balance unchanged, no record |
| `WithdrawMissingAccountFails` | `failed()`, `"account not found"` |
| `TransferMovesFundsAtomically` | Source decreases, destination increases by the same amount; one record; total conserved |
| `TransferInsufficientFundsFails` | Both balances unchanged; no record |
| `TransferMissingSourceFails` | `failed()`, non-empty error |
| `TransferMissingDestinationFails` | `failed()`, non-empty error |
| `TransferToSameAccountFails` | Processor validation rejects it; no balance change |
| `BalanceReturnsCurrentMoney` | After deposit/withdraw, `balance()` matches the repository |
| `BalanceMissingAccountFails` | `failed()`, `"account not found"` |
| `HistoryReturnsAllRecordsForAccount` | Deposit, withdraw, and both sides of a transfer appear for the correct accounts |
| `HistoryUnknownAccountReturnsEmptySuccess` | `ok()` with an empty vector |
| `FreezeRejectsDeposit` | Freeze, then deposit → `failed()`; balance unchanged |
| `FreezeRejectsWithdraw` | Freeze, then withdraw → `failed()`; balance unchanged |
| `FreezeRejectsTransfer` | Freeze source, then transfer → `failed()`; both balances unchanged |
| `FreezeMissingAccountFails` | `failed()`, non-empty error |
| `UnfreezeRestoresTransactions` | Freeze → deposit fails; unfreeze → deposit succeeds and balance increases |
| `UnfreezeMissingAccountFails` | `failed()`, non-empty error |
| `FreezeAndUnfreezeWriteAuditEntries` | `"account.frozen"` / `"account.unfrozen"` present for the successful transitions |
| `SuccessfulMutationWritesExactlyOneTransactionRecord` | Deposit/withdraw/transfer each append exactly one record; no duplicates |
| `FailedMutationWritesNoTransactionRecord` | All failing transaction paths leave the transaction repository unchanged |
| `EveryFailureHasNonEmptyError` | Exercise missing account, insufficient funds, frozen account; assert `!error().empty()` on every failure |
| `ConcurrentDepositsConserveMoney` | 4 threads deposit into distinct accounts through one BankingService; final total equals initial plus all deposits; no lost updates |

## Suite `AuditService` — in `tests/test_integration.cpp`

These stay in the integration test file because the deliverable list fixes the four test files.

| Test | Invariant |
|---|---|
| `FindWithEmptyFilterReturnsAllEntries` | Every appended entry is returned |
| `FindByActorFilters` | Only entries for the requested actor are returned |
| `FindByActionFilters` | Only exact-action matches are returned |
| `FindSinceFilters` | Entries older than the timestamp are excluded; newer ones included |
| `FindUnknownActorReturnsEmptySuccess` | `ok()` with an empty vector |
| `RepositoryThrowBecomesFailure` | A throwing fake `AuditRepository` yields `failed()` with a non-empty error; AuditService never throws |

## Suite `InterestService` — in `tests/test_integration.cpp`

| Test | Invariant |
|---|---|
| `StubAccrualReturnsSuccess` | `accrue_monthly_interest().ok()` |
| `StubAccrualDoesNotChangeBalances` | All account balances and transaction count unchanged |
| `StubAccrualDoesNotWriteAudit` | Audit repository remains unchanged |

The stub is constructed with only the account and audit repositories. No `TransactionProcessor` is involved until accrual is implemented.

## Suite `Integration` — `tests/test_integration.cpp`

Uses one `IntegrationHarness` (the `ServiceHarness` above) with two customers and two accounts.

| Test | Invariant |
|---|---|
| `FullLifecycleLoginDepositWithdrawTransferHistoryBalance` | Login both customers; deposit, withdraw, transfer; history for each account is correct; balances are correct; all calls return `ok()` |
| `TotalMoneyIsConservedAcrossLifecycle` | Sum of all balances after operations equals the initial sum plus deposits minus withdrawals (transfers cancel internally) |
| `AuditTrailContainsLoginAndTransactionEvents` | `"auth.login"` entries exist; `"txn.completed"` entries exist and match the number of successful mutations |
| `HistoryMatchesLedger` | For each account, every record returned by `history()` corresponds to a transaction that involved that account; no phantom records |
| `FreezeUnfreezeInLifecycle` | Freeze an account, observe transaction rejection, unfreeze, observe success; audit contains both status actions |
| `LoginThenLoggedOutTokenIsInvalid` | Login through AuthService, logout, then SessionManager validation fails |
| `ConcurrentTransfersThroughServiceConserveMoney` | Multiple threads transfer between the two accounts; total balance is invariant; no negative balances; no transaction/audit loss. **This tests BankingService correctness (the service-level `op_mutex_` + synchronous `process_one` path), not TransactionProcessor concurrency; processor-level concurrency is covered by the existing stress tests.** |
| `BalanceReadsAreConsistentWithFinalHistory` | After concurrent operations, `balance()` values agree with a fresh repository read; history counts equal committed transaction records |

## Which tests need concurrency / TSan

Run the full suite under `tsan`; the whole binary must be TSan-clean. The tests that specifically exist for concurrency are:

- `SessionManager.ConcurrentTokenCreationIsUnique` — 4 threads × 100 tokens.
- `SessionManager.ConcurrentValidateAndRevokeDoNotRace`.
- `BankingService.ConcurrentDepositsConserveMoney`.
- `Integration.ConcurrentTransfersThroughServiceConserveMoney` — service correctness under concurrent callers, not a processor-concurrency benchmark.
- `AuthService` tests do not require threads; they run under TSan harmlessly.
- `AuditService`, `InterestService`, and single-threaded BankingService tests are TSan-neutral but run in the same binary.

The deterministic concurrency tests use `std::barrier`/`std::latch` and `std::jthread`, matching the existing concurrency test style. No `sleep_for` is used as a synchronization primitive.
