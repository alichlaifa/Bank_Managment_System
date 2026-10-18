#pragma once

#include "domain/Account.h"
#include "domain/Money.h"
#include "domain/Ostream.h"
#include "domain/Types.h"
#include "persistence/InMemoryAccountRepository.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
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