# Design Decisions

Notes on why the code is shaped the way it is. Written first person, in the
order I would explain it to another engineer.

## Money is integer cents, never floating point

Every money value is an `int64_t` count of cents. Floating point never appears
on the financial path, because `0.1 + 0.2` is not exactly `0.3` and a bank run
on floats produces balances that do not add up. Integer cents are exact, sort
lexicographically, and are trivial to compare and persist.

Overflow is guarded explicitly: `Money::plus` checks against
`std::numeric_limits<int64_t>::max()` before adding, and `Money::minus` refuses
to go below zero. These are property checks, not style — a `Money` object can
never hold a negative or overflowing balance.

## Transactions are std::variant, not inheritance

A transaction is exactly one of `Deposit`, `Withdraw`, or `Transfer`. Modeling
this as `std::variant` makes invalid states unrepresentable: a transfer always
carries both sides, and money fields never leak across types. There is no
`UnknownTransaction` and no empty base-class pointer.

The set of transaction kinds is closed and small. If new kinds were added
frequently, or if transactions had shared behavior worth overriding, an
inheritance hierarchy would earn its keep. Here value semantics win: variants
are copied and stored by value, which is exactly what the WAL and repositories
need, with no object lifetimes to manage.

## WAL before apply

A transaction is appended to the append-only journal *before* any balance is
mutated. The failure mode this prevents is a crash halfway through a bank
operation: with WAL-before-apply there is always a durable record of what was
in flight, so recovery can replay committed work and skip or discard the rest.

Recovery is: load the latest snapshot, replay the WAL records after the
snapshot's LSN, and apply only the committed ones. A torn transaction is simply
an append that never completed — the committed prefix of the file is intact and
a partial final line is truncated on the next write.

The WAL record framing uses a fixed-width header so `mark_committed` can flip
the state byte in place without rewriting the payload. The header state byte is
authoritative; the `"state"` field embedded in the JSON payload is
informational. The header is written as `'0' + int(state)`, which means the
integer values of `TransactionState` are **ABI**: they are literally written to
disk, so the enum must never be reordered.

## Exceptions at the value-object boundary, Result at the service boundary

Domain value objects throw `std::domain_error` for rule violations (insufficient
funds, transaction on a frozen account) and `std::invalid_argument` for bad
construction. These are the "whoops, that's not allowed" cases that a caller
invokes only by asking wrong.

The service layer is planned to return `Result<T>` instead, so services expose
expected failures (wrong PIN, overdraft) as values callers must check, rather
than as control flow through the exception machinery. `Result<T>` is currently
uninstantiated — no services exist yet — and is being kept for the Auth/API
milestones. Note that `Result<void>` keys `ok()` off the error string rather
than a value, so the two specializations disagree slightly on what "ok" means.

## Why a hand-rolled JSON mini-library

The WAL and snapshot formats need serialization, and pulling in a general JSON
library (nlohmann, RapidJSON, ...) adds a dependency for a format that is
deliberately tiny. Our JSON model is integer-only by design: money is an
`int64_t`, timestamps are integer milliseconds, so floats and booleans are not
modeled at all — including them would only be a foot-gun. The parser is
recursive-descent, fails whole-on-any-malformed-input, and is fully covered by
tests. It is not a general JSON library, and it is not intended to become one.

## What I deliberately did NOT do

- **No connection pool for SQLite** — the design is single-writer, and one
  connection with the concurrency boundary above it is enough at this scale.
- **No async I/O** — threads are sufficient for the expected volume.
- **No distributed coordination** — this is a single-process system.
- **No metrics or monitoring**.
- **No external JSON dependency** — see the JSON decision above.

## Known limitations

- **The WAL uses `flush`, not `fsync`.** A flushed stream is not true
  durability: on a power failure the OS can still lose the last writes. Closing
  this gap needs an `open(2)` descriptor and an `fsync` on append, which belongs
  to the transaction processor milestone.
- **WAL append is single-threaded by contract.** The thread pool milestone will
  add internal locking; until then the caller must funnel writes.
- **In-memory repositories are not thread-safe by design.** Concurrency control
  lives in the transaction processor above them.
- **The whole WAL is read into memory on open.** Fine for tests and small
  journals; not a production-scale strategy.
- **`TransactionState` defines `Applied` / `WalWritten` / `Failed` /
  `RolledBack` but the code currently only emits `Pending` and `Committed`** —
  the intermediate states become reachable when the transaction processor
  lands.
