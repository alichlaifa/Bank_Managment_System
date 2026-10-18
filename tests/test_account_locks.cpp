#include <gtest/gtest.h>

#include "TestSupport.h"
#include "concurrency/AccountLockManager.h"
#include "concurrency/ScopedAccountLock.h"
#include "concurrency/ScopedAccountLocks.h"

#include <chrono>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace bank;
using namespace std::chrono_literals;

TEST(ScopedAccountLock, LocksOnConstructionUnlocksOnDestruction)
{
    // Shared so a thread detached on timeout keeps the mutex alive.
    auto shared_mutex = std::make_shared<std::mutex>();

    {
        ScopedAccountLock guard(*shared_mutex);

        // Held while the guard is alive (same-thread try_lock on a locked
        // std::mutex reports failure).
        ASSERT_FALSE(shared_mutex->try_lock());

        // A second thread must block for as long as the guard is held.
        ASSERT_FALSE(completes_within([m = shared_mutex] {
                         m->lock();
                         m->unlock();
                     },
                     50ms));
    }

    // Unlocked on destruction: a waiting thread can now proceed.
    ASSERT_TRUE(completes_within([m = shared_mutex] {
                    m->lock();
                    m->unlock();
                },
                2000ms));
}

TEST(ScopedAccountLock, MoveTransfersOwnership)
{
    std::mutex source;
    std::mutex target;

    // Move construction: `staging` locks, ownership moves to `moved`, and
    // destroying the moved-from `staging` must NOT release the mutex.
    std::optional<ScopedAccountLock> staging;
    staging.emplace(source);
    ScopedAccountLock moved(std::move(*staging));
    staging.reset();  // destroys the moved-from guard
    ASSERT_FALSE(source.try_lock());

    {
        // Move assignment: `other` releases `target` and takes `source`.
        ScopedAccountLock other(target);
        ASSERT_FALSE(target.try_lock());

        other = std::move(moved);
        ASSERT_TRUE(target.try_lock());  // released by move assignment
        target.unlock();
        ASSERT_FALSE(source.try_lock());  // held by `other` now
    }

    // Released when the final owner is destroyed.
    ASSERT_TRUE(source.try_lock());
    source.unlock();
}

TEST(ScopedAccountLock, MutexIsUnlockedOnScopeExit)
{
    std::mutex m;
    bool caught = false;

    try {
        ScopedAccountLock guard(m);
        ASSERT_FALSE(m.try_lock());
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
        caught = true;
    }

    ASSERT_TRUE(caught);

    // The guard's destructor ran during unwinding; the mutex is free again.
    ASSERT_TRUE(m.try_lock());
    m.unlock();
}

TEST(AccountLockManager, LockSingleAccountMutualExclusion)
{
    AccountLockManager manager;
    CriticalSectionProbe probe;
    constexpr int kIterations = 500;

    auto worker = [&] {
        for (int i = 0; i < kIterations; ++i) {
            auto guard = manager.lock(AccountId{7});
            probe.enter();
            std::this_thread::yield();  // widen the race window
            probe.leave();
        }
    };

    std::latch start{2};
    std::jthread a{[&] {
        start.arrive_and_wait();
        worker();
    }};
    std::jthread b{[&] {
        start.arrive_and_wait();
        worker();
    }};

    a.join();
    b.join();

    ASSERT_EQ(probe.max_depth(), 1) << "two threads were inside the critical section at once";
}

TEST(AccountLockManager, LockTwoAccountsSortsAscending)
{
    // Shared so a thread detached on timeout keeps the objects alive.
    auto manager = std::make_shared<AccountLockManager>();
    auto a_holds = std::make_shared<std::latch>(1);
    auto b_may_start = std::make_shared<std::latch>(1);

    const bool ok = completes_within(
        [manager, a_holds, b_may_start] {
            // A locks (1, 5) — ascending.
            std::jthread a{[manager, a_holds, b_may_start] {
                auto guard = manager->lock(AccountId{1}, AccountId{5});
                a_holds->count_down();
                b_may_start->wait();
            }};
            a_holds->wait();

            // B requests the same pair REVERSED. With sorted acquisition it
            // blocks on id 1 (held by A) and A releases after b_may_start;
            // without sorting this is an ABBA deadlock.
            std::jthread b{[manager, b_may_start] {
                b_may_start->count_down();
                auto guard = manager->lock(AccountId{5}, AccountId{1});
            }};
            a.join();
            b.join();
        },
        2000ms);

    ASSERT_TRUE(ok) << "possible deadlock in two-account lock ordering";
}

TEST(AccountLockManager, LockSameAccountTwiceYieldsOneMutex)
{
    AccountLockManager manager;

    auto guard = manager.lock(AccountId{9}, AccountId{9});

    ASSERT_EQ(guard.size(), 1u);
    ASSERT_EQ(guard.ids()[0], AccountId{9});
    ASSERT_EQ(guard.ids()[1], AccountId{9});
    ASSERT_EQ(manager.mutex_count(), 1u);
}

TEST(AccountLockManager, MutexesAreCreatedOnDemand)
{
    AccountLockManager manager;
    ASSERT_EQ(manager.mutex_count(), 0u);

    {
        auto guard = manager.lock(AccountId{42});
        ASSERT_EQ(manager.mutex_count(), 1u);
    }

    {
        auto guard = manager.lock(AccountId{42});  // same id: no new mutex
        ASSERT_EQ(manager.mutex_count(), 1u);
    }

    {
        auto guard = manager.lock(AccountId{7});
        ASSERT_EQ(manager.mutex_count(), 2u);
    }
}

TEST(AccountLockManager, DistinctAccountsHaveDistinctMutexes)
{
    // Shared so a thread detached on timeout keeps the objects alive.
    auto manager = std::make_shared<AccountLockManager>();
    auto a_holds = std::make_shared<std::latch>(1);
    auto b_done = std::make_shared<std::latch>(1);

    std::jthread a{[manager, a_holds, b_done] {
        auto guard = manager->lock(AccountId{1});
        a_holds->count_down();
        // Hold id 1 until B is done, but never longer than 2 s so a failing
        // run still unwinds (libstdc++13 latch has no timed wait; poll).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (!b_done->try_wait() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }};
    a_holds->wait();

    // B locks a different id and must NOT wait for A to release.
    ASSERT_TRUE(completes_within([manager, b_done] {
                    auto guard = manager->lock(AccountId{2});
                    b_done->count_down();
                },
                500ms))
        << "distinct accounts appear to share one mutex";

    a.join();
}
