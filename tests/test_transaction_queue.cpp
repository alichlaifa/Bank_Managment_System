#include <gtest/gtest.h>

#include "TestSupport.h"
#include "concurrency/TransactionQueue.h"

#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <variant>

using namespace bank;
using namespace std::chrono_literals;

namespace {

// Amount of a Deposit or Withdraw transaction (transfers not used here).
int64_t amount_of_tx(const Transaction& txn)
{
    return std::visit([](const auto& value) { return value.amount.cents(); }, txn);
}

AccountId account_of_tx(const Transaction& txn)
{
    return std::visit(
        [](const auto& value) -> AccountId {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Transfer>) {
                return value.from;
            } else {
                return value.account_id;
            }
        },
        txn);
}

Transaction deposit_tx(AccountId id, int64_t cents)
{
    return Deposit{id, Money{cents}};
}

} // namespace

TEST(TransactionQueue, FifoOrder)
{
    TransactionQueue queue;

    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 1)));
    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 2)));
    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 3)));

    auto a = queue.try_pop();
    auto b = queue.try_pop();
    auto c = queue.try_pop();

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(amount_of_tx(*a), 1);
    EXPECT_EQ(amount_of_tx(*b), 2);
    EXPECT_EQ(amount_of_tx(*c), 3);
}

TEST(TransactionQueue, TryPopEmptyReturnsNullopt)
{
    TransactionQueue queue;
    ASSERT_FALSE(queue.try_pop().has_value());
}

TEST(TransactionQueue, EnqueueAfterCloseFails)
{
    TransactionQueue queue;
    queue.close();

    EXPECT_FALSE(queue.enqueue(deposit_tx(AccountId{1}, 100)));
    EXPECT_EQ(queue.size(), 0u);
}

TEST(TransactionQueue, WaitPopDrainsThenNulloptAfterClose)
{
    TransactionQueue queue;
    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 10)));
    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{2}, 20)));
    queue.close();

    // Closed but non-empty: wait_pop still drains in FIFO order.
    auto a = queue.wait_pop(std::stop_token{});
    auto b = queue.wait_pop(std::stop_token{});
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(amount_of_tx(*a), 10);
    EXPECT_EQ(account_of_tx(*b), AccountId{2});

    // Closed and empty: nullopt immediately (no block).
    auto c = queue.wait_pop(std::stop_token{});
    ASSERT_FALSE(c.has_value());
}

TEST(TransactionQueue, WaitPopWokenByStopToken)
{
    // Shared so a thread that never wakes (broken impl) can be detached
    // without dangling references.
    auto queue = std::make_shared<TransactionQueue>();
    auto result = std::make_shared<std::optional<Transaction>>();
    auto about_to_wait = std::make_shared<std::latch>(1);
    auto source = std::make_shared<std::stop_source>();

    std::thread consumer{[queue, result, about_to_wait, source] {
        about_to_wait->count_down();
        *result = queue->wait_pop(source->get_token());
    }};

    about_to_wait->wait();
    // Give the consumer a moment to actually enter the wait.
    std::this_thread::sleep_for(20ms);

    source->request_stop();

    const bool finished = completes_within([&consumer] { consumer.join(); }, 2000ms);
    if (!finished) {
        consumer.detach();  // avoid joining a stuck thread at teardown
    }
    ASSERT_TRUE(finished) << "wait_pop did not wake on stop_token";
    ASSERT_FALSE(result->has_value());
}

TEST(TransactionQueue, SizeAndClosedQueries)
{
    TransactionQueue queue;

    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.closed());

    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 5)));
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_FALSE(queue.closed());

    ASSERT_TRUE(queue.enqueue(deposit_tx(AccountId{1}, 5)));
    EXPECT_EQ(queue.size(), 2u);

    (void)queue.try_pop();
    EXPECT_EQ(queue.size(), 1u);

    queue.close();
    EXPECT_TRUE(queue.closed());
    EXPECT_EQ(queue.size(), 1u);  // close does not discard items

    queue.close();  // idempotent
    EXPECT_TRUE(queue.closed());
    EXPECT_EQ(queue.size(), 1u);
}
