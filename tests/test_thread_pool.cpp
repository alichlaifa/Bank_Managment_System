#include <gtest/gtest.h>

#include "TestSupport.h"
#include "concurrency/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace bank;
using namespace std::chrono_literals;

TEST(ThreadPool, TasksExecute)
{
    std::atomic<int> counter{0};
    {
        ThreadPool pool{4};
        for (int i = 0; i < 8; ++i) {
            ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
        }
        pool.stop();
    }
    ASSERT_EQ(counter.load(), 8);
}

TEST(ThreadPool, PendingCountsUnstartedTasks)
{
    // 0-thread pool: nothing starts until stop(), so pending() is exact.
    std::atomic<int> counter{0};
    ThreadPool pool{0};

    ASSERT_TRUE(pool.submit([&] { counter.fetch_add(1); }));
    ASSERT_TRUE(pool.submit([&] { counter.fetch_add(1); }));
    ASSERT_EQ(pool.pending(), 2u);
    ASSERT_EQ(counter.load(), 0);

    pool.stop();
    ASSERT_EQ(pool.pending(), 0u);
    ASSERT_EQ(counter.load(), 2);
}

TEST(ThreadPool, SubmitAfterStopReturnsFalse)
{
    ThreadPool pool{2};
    pool.stop();

    ASSERT_FALSE(pool.submit([] {}));
}

TEST(ThreadPool, StopIsIdempotent)
{
    ThreadPool pool{2};
    ASSERT_TRUE(pool.submit([] {}));
    pool.stop();
    pool.stop();  // must not crash or hang
    pool.stop();
}

TEST(ThreadPool, StopDrainsQueue)
{
    // 0-thread pool: stop() drains synchronously on the calling thread.
    std::vector<int> order;
    ThreadPool pool{0};

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(pool.submit([&order, i] { order.push_back(i); }));
    }
    ASSERT_EQ(pool.pending(), 5u);

    pool.stop();

    ASSERT_EQ(order.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

TEST(ThreadPool, ZeroThreadPoolRunsNothingUntilStop)
{
    // Chosen semantics for thread_count == 0: submit() still queues work,
    // but no worker exists, so tasks do not run until stop() drains them
    // synchronously on the caller's thread. stop() never drops queued work.
    std::atomic<int> counter{0};
    ThreadPool pool{0};

    ASSERT_TRUE(pool.submit([&] { counter.fetch_add(1); }));
    ASSERT_TRUE(pool.submit([&] { counter.fetch_add(1); }));
    std::this_thread::sleep_for(20ms);
    ASSERT_EQ(counter.load(), 0);  // nothing ran yet

    pool.stop();
    ASSERT_EQ(counter.load(), 2);  // drained by stop()
}

TEST(ThreadPool, ManyThreadsManyTasks)
{
    constexpr int kThreads = 8;
    constexpr int kTasksPerThread = 1000;

    std::atomic<int> counter{0};
    ThreadPool pool{static_cast<std::size_t>(kThreads)};

    std::vector<std::jthread> submitters;
    submitters.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        submitters.emplace_back([&pool, &counter] {
            for (int i = 0; i < kTasksPerThread; ++i) {
                ASSERT_TRUE(pool.submit([&counter] { counter.fetch_add(1); }));
            }
        });
    }
    submitters.clear();  // join all submitters

    pool.stop();
    ASSERT_EQ(counter.load(), kThreads * kTasksPerThread);
    ASSERT_EQ(pool.pending(), 0u);
}
