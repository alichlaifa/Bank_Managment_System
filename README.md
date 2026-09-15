# Bank Management System

A C++20 concurrency-focused bank management system, built as a portfolio
project demonstrating layered architecture, per-account locking, WAL-based
durability, and modern C++ idioms.

## Status

- [x] UML design
- [ ] CMake scaffold
- [ ] Domain model
- [ ] Persistence (WAL + snapshot)
- [ ] Concurrency (thread pool, per-account locks)
- [ ] Auth + audit log
- [ ] TCP server + CLI client
- [ ] Tests, sanitizers, CI

## Architecture

Full diagram set: [`docs/uml/`](docs/uml/)

### Domain Model
![Domain](docs/uml/class/04a_class_domain.svg)

### Transfer Sequence
![Transfer](docs/uml/sequence/14_seq_transfer.svg)

### Transaction State Machine
![State](docs/uml/state/07_state_transaction.svg)

### Component View
![Components](docs/uml/component/03_component.svg)
