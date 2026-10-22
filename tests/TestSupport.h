#pragma once

#include "concurrency/TransactionProcessor.h"
#include "core/TimeUtil.h"
#include "domain/Account.h"
#include "domain/Customer.h"
#include "domain/Money.h"
#include "domain/Ostream.h"
#include "domain/Types.h"
#include "persistence/InMemoryAccountRepository.h"
#include "persistence/InMemoryAuditRepository.h"
#include "persistence/InMemoryCustomerRepository.h"
#include "persistence/InMemoryTransactionRepository.h"
#include "persistence/WalLogger.h"
#include "services/AuditService.h"
#include "services/AuthService.h"
#include "services/BankingService.h"
#include "services/InterestService.h"
#include "services/SessionManager.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// RAII owner of a disposable temp directory shared by persistence tests.
class TempDir {
public:
    explicit TempDir(const std::string& tag)
    {
        path = std::filesystem::temp_directory_path();
        path /= "bank_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(next_id_++);
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::filesystem::remove_all(path);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path path;

private:
    inline static int next_id_ = 0;
};

// Controllable wall clock for session-expiry tests.
struct FakeClock {
    bank::timeutil::Clock::time_point current{bank::timeutil::now()};

    [[nodiscard]] bank::timeutil::Clock::time_point now() const noexcept
    {
        return current;
    }

    void advance(std::chrono::seconds delta)
    {
        current += delta;
    }
};

// Mutual-exclusion probe: enter() increments an atomic depth and records the
// maximum depth ever observed. After a contention run, assert that
// max_depth() == 1 — a value above 1 means two threads were inside at once.
class CriticalSectionProbe {
public:
    void enter() noexcept
    {
        const int depth = depth_.fetch_add(1) + 1;
        int observed = max_depth_.load();
        while (depth > observed && !max_depth_.compare_exchange_weak(observed, depth)) {
        }
    }

    void leave() noexcept { depth_.fetch_sub(1); }

    [[nodiscard]] int depth() const noexcept { return depth_.load(); }
    [[nodiscard]] int max_depth() const noexcept { return max_depth_.load(); }

private:
    std::atomic<int> depth_{0};
    std::atomic<int> max_depth_{0};
};

// Runs `fn` on a std::jthread and reports whether it finished within `limit`.
// On timeout the thread is detached (not joined) and false is returned; `fn`
// must not touch objects whose lifetime ends when the calling test ends
// unless it owns copies of them (capture by value). Used by tests as a
// bounded "did not deadlock / did not hang" check.
template <typename F>
[[nodiscard]] bool completes_within(F&& fn, std::chrono::milliseconds limit)
{
    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();

    std::jthread worker(
        [p = std::move(promise), func = std::forward<F>(fn)]() mutable {
            try {
                func();
                p.set_value(true);
            } catch (...) {
                p.set_value(false);
            }
        });

    if (future.wait_for(limit) == std::future_status::ready) {
        return future.get();
    }
    worker.detach();
    return false;
}

// Seeds `repo` with `count` Active accounts (ids 1..count), all owned by
// CustomerId{1}, each with balance `initial`. Returns the created AccountIds
// in ascending order. Used by processor/stress test fixtures.
inline std::vector<bank::AccountId> seed_accounts(bank::InMemoryAccountRepository& repo,
                                                  std::size_t count,
                                                  bank::Money initial)
{
    std::vector<bank::AccountId> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        bank::AccountId id{static_cast<uint64_t>(i + 1)};
        bank::Account account{id, bank::CustomerId{1}, initial,
                              bank::AccountStatus::Active, 0};
        repo.save(account);
        ids.push_back(id);
    }
    return ids;
}

// Deterministic PIN fixture. Reuses one PasswordHasher derivation per PIN
// so service tests do not pay PBKDF2 cost repeatedly.
inline bank::PasswordHash pin_hash_for(std::string_view pin)
{
    static std::mutex cache_mutex;
    static std::map<std::string, bank::PasswordHash> cache;

    const std::string key{pin};
    std::lock_guard<std::mutex> guard{cache_mutex};
    const auto found = cache.find(key);
    if (found != cache.end()) {
        return found->second;
    }
    bank::PasswordHash hash = bank::PasswordHasher::create_with_salt(
        key, "00112233445566778899aabbccddeeff");
    cache.emplace(key, hash);
    return hash;
}

inline bank::Customer make_customer(bank::CustomerId id,
                                    std::string_view pin,
                                    bank::Role role)
{
    return bank::Customer{id,
                          "customer-" + std::to_string(id.value()),
                          pin_hash_for(pin),
                          role};
}

// Seeds one customer into a CustomerRepository.
inline void seed_customer(bank::InMemoryCustomerRepository& repo,
                          bank::CustomerId id,
                          std::string_view pin = "1234",
                          bank::Role role = bank::Role::Customer)
{
    repo.save(make_customer(id, pin, role));
}

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

inline bank::Account make_account(
    bank::AccountId id,
    bank::CustomerId owner,
    bank::Money balance,
    bank::AccountStatus status = bank::AccountStatus::Active)
{
    return bank::Account{id, owner, balance, status, 0};
}

// Seeds one account into an AccountRepository and returns its id.
inline bank::AccountId seed_account(
    bank::InMemoryAccountRepository& repo,
    bank::AccountId id,
    bank::CustomerId owner,
    bank::Money initial,
    bank::AccountStatus status = bank::AccountStatus::Active)
{
    repo.save(make_account(id, owner, initial, status));
    return id;
}

// Money and audit helpers.
inline bank::Money total_balance(const bank::AccountRepository& accounts)
{
    bank::Money total = bank::Money::zero();
    for (const bank::Account& account : accounts.all()) {
        total = total + account.balance();
    }
    return total;
}

inline std::vector<std::string>
audit_actions(const bank::AuditRepository& audit)
{
    std::vector<std::string> actions;
    for (const bank::AuditEntry& entry : audit.find(bank::AuditFilter{})) {
        actions.push_back(entry.action);
    }
    return actions;
}

inline bool has_audit(const bank::AuditRepository& audit,
                      std::string_view action)
{
    bank::AuditFilter filter;
    filter.action = std::string{action};
    return !audit.find(filter).empty();
}

inline std::size_t
transaction_count(const bank::TransactionRepository& transactions)
{
    return transactions.all().size();
}

// Full service harness. Declaration order is construction order.
struct ServiceHarness {
    explicit ServiceHarness(std::size_t accounts_count = 0,
                            bank::Money initial = bank::Money{0})
        : dir{"services"},
          customers{},
          accounts{},
          transactions{},
          audit{},
          wal{dir.path / "wal.log"},
          processor{0, accounts, transactions, audit, wal},
          sessions{},
          auth{customers, sessions, audit},
          banking{processor, accounts, transactions, audit},
          audit_query{audit},
          interest{accounts, audit}
    {
        for (std::size_t i = 0; i < accounts_count; ++i) {
            seed_account(accounts,
                         bank::AccountId{static_cast<uint64_t>(i + 1)},
                         bank::CustomerId{1},
                         initial);
        }
    }

    TempDir dir;                                        // must be first
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

// Releases `threads` workers simultaneously and has each create `per_thread`
// sessions. Returns every token produced, in completion order.
inline std::vector<std::string>
create_tokens_concurrently(bank::SessionManager& sessions,
                           bank::CustomerId customer,
                           int threads,
                           int per_thread)
{
    std::vector<std::string> tokens;
    tokens.reserve(static_cast<std::size_t>(threads) *
                   static_cast<std::size_t>(per_thread));

    std::mutex tokens_mutex;
    std::barrier sync{static_cast<std::ptrdiff_t>(threads)};
    std::vector<std::jthread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&sessions, customer, per_thread, &sync,
                              &tokens, &tokens_mutex] {
            std::vector<std::string> local;
            local.reserve(static_cast<std::size_t>(per_thread));

            sync.arrive_and_wait();

            for (int j = 0; j < per_thread; ++j) {
                local.push_back(
                    sessions.create(customer, bank::Role::Customer).token);
            }

            std::lock_guard<std::mutex> guard{tokens_mutex};
            tokens.insert(tokens.end(), local.begin(), local.end());
        });
    }

    // std::jthread has no join(); clearing the vector destroys each worker,
    // which requests stop and joins before tokens is returned.
    workers.clear();
    return tokens;
}