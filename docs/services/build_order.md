# 4. `docs/services/build_order.md`

Each step is a vertical slice: add the listed source/test files to CMake during implementation, build, run the focused suite, then run the full existing suite under `dev`. No CMake edits are made in this plan pass. No changes to the processor, thread pool, queue, lock manager, WalLogger, or any `.puml` file.

CMake target additions (for reference only, applied during implementation):

- `bank` target: `src/services/SessionManager.cpp`, `AuthService.cpp`, `BankingService.cpp`, `InterestService.cpp`, `AuditService.cpp`.
- `bank_tests` target: `tests/test_session_manager.cpp`, `test_auth_service.cpp`, `test_banking_service.cpp`, `test_integration.cpp`.

No new third-party libraries.

## Step 1 — `SessionManager` + tests

Files:

- `src/services/SessionManager.h`
- `src/services/SessionManager.cpp`
- `tests/test_session_manager.cpp`
- `tests/TestSupport.h` additions: `FakeClock`, `create_tokens_concurrently` (service harness not needed yet)

Add `SessionManager.cpp` to `bank`; add `test_session_manager.cpp` to `bank_tests`.

The header carries the explicit note that the eight-hour default TTL is **chosen for the demo**.

Acceptance:

```bash
cmake --preset dev
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter='SessionManager.*'
ctest --preset dev --output-on-failure
```

Expected: all `SessionManager.*` tests PASS; the pre-existing 85 tests still PASS; ASan/UBSan report nothing. Then:

```bash
cmake --preset tsan
cmake --build --preset tsan --target bank_tests
./build/tsan/bank_tests --gtest_filter='SessionManager.*'
```

Expected: all `SessionManager.*` tests PASS with no TSan report; `ConcurrentTokenCreationIsUnique` sees exactly 400 unique tokens.

## Step 2 — `AuthService` + tests

Files:

- `src/services/AuthService.h`
- `src/services/AuthService.cpp`
- `tests/test_auth_service.cpp`
- `tests/TestSupport.h` additions: `pin_hash_for`, `make_customer`, `seed_customer`, `ThrowingAuditRepository`, partial `ServiceHarness`

Add `AuthService.cpp` to `bank`; add `test_auth_service.cpp` to `bank_tests`.

Acceptance:

```bash
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter='AuthService.*:SessionManager.*'
ctest --preset dev --output-on-failure
```

Expected: all `AuthService.*` and `SessionManager.*` tests PASS; both login-failure paths return exactly `"invalid credentials"`; both write `"auth.login_failed"`; `LoginSucceedsEvenIfAuditFails` proves a thrown audit append does not deny a valid login; the pre-existing suite is still green; ASan/UBSan clean.

Optional TSan confirmation:

```bash
cmake --build --preset tsan --target bank_tests
./build/tsan/bank_tests --gtest_filter='AuthService.*:SessionManager.*'
```

Expected: no TSan report.

## Step 3 — `BankingService` + tests

Files:

- `src/services/BankingService.h`
- `src/services/BankingService.cpp`
- `tests/test_banking_service.cpp`
- `tests/TestSupport.h` additions: `make_account`, `seed_account`, `total_balance`, `audit_actions`, `has_audit`, `transaction_count`, full `ServiceHarness`

Add `BankingService.cpp` to `bank`; add `test_banking_service.cpp` to `bank_tests`.

The `op_mutex_` member carries the explicit serialization tradeoff comment from `interfaces.md`: service-wide serialization is required because the processor's private `AccountLockManager` cannot be shared in this milestone, and direct `submit()` callers bypass the guarantee.

Acceptance:

```bash
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter='BankingService.*'
ctest --preset dev --output-on-failure
```

Expected: all `BankingService.*` tests PASS; every failing path has a non-empty error and leaves balances/records unchanged; frozen accounts reject deposit, withdraw, and transfer; the pre-existing suite plus Steps 1–2 remain green; ASan/UBSan clean.

TSan check for the concurrent BankingService case:

```bash
cmake --build --preset tsan --target bank_tests
./build/tsan/bank_tests --gtest_filter='BankingService.*'
```

Expected: all PASS; `ConcurrentDepositsConserveMoney` is TSan-clean.

## Step 4 — `InterestService` stub + `AuditService` + their tests

Files:

- `src/services/InterestService.h`
- `src/services/InterestService.cpp`
- `src/services/AuditService.h`
- `src/services/AuditService.cpp`
- `tests/test_integration.cpp` created here with only the `AuditService` and `InterestService` suites
- `tests/TestSupport.h` additions: `ServiceHarness` completed with `audit_query` and `interest`

Add both `.cpp` files to `bank`; add `test_integration.cpp` to `bank_tests`.

`InterestService` is constructed with only `AccountRepository&` and `AuditRepository&`. It does not take `TransactionProcessor&` in this milestone; that parameter is added back when `accrue_monthly_interest()` is implemented.

Acceptance:

```bash
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter='AuditService.*:InterestService.*'
ctest --preset dev --output-on-failure
```

Expected: all `AuditService.*` and `InterestService.*` tests PASS; InterestService changes no balances and writes no audit; AuditService filter helpers return the expected subsets; the pre-existing suite plus Steps 1–3 remain green; ASan/UBSan clean.

## Step 5 — End-to-end integration test

Files:

- `tests/test_integration.cpp` extended with the `Integration` suite
- No new `src` files
- No CMake source-list change beyond the file already added in Step 4

Acceptance:

```bash
cmake --build --preset dev --target bank_tests
./build/dev/bank_tests --gtest_filter='Integration.*'
./build/dev/bank_tests
ctest --preset dev --output-on-failure
```

Expected: all `Integration.*` tests PASS; the full `bank_tests` binary PASSes; money is conserved through login → deposit → withdraw → transfer → history → balance; audit entries are present; ASan/UBSan clean.

`Integration.ConcurrentTransfersThroughServiceConserveMoney` is a BankingService correctness test under concurrent callers, not a TransactionProcessor concurrency test. Processor-level concurrency remains covered by the existing stress suite.

Then run the other presets:

```bash
cmake --preset release
cmake --build --preset release --target bank_tests
ctest --preset release --output-on-failure

cmake --preset tsan
cmake --build --preset tsan --target bank_tests
ctest --preset tsan --output-on-failure
```

Expected: `release` and `tsan` are green; no TSan race reports on the concurrent SessionManager, BankingService, or Integration tests; no sanitizer findings. This is the services-milestone exit criterion.
