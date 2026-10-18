#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>

#include "domain/Transaction.h"

namespace bank {

// Unbounded FIFO of Transactions. Thread-safe.
// Uses condition_variable_any so waits can be interrupted by a stop_token.
// Non-copyable, non-movable.
class TransactionQueue {
public:
    TransactionQueue() = default;
    ~TransactionQueue() = default;

    TransactionQueue(const TransactionQueue&) = delete;
    TransactionQueue& operator=(const TransactionQueue&) = delete;
    TransactionQueue(TransactionQueue&&) = delete;
    TransactionQueue& operator=(TransactionQueue&&) = delete;

    // Returns false once close() has been called.
    bool enqueue(Transaction txn);

    // Blocks until an item is available, or returns nullopt when the queue
    // is closed AND empty, or when `token` is stopped and the queue is
    // observed empty. FIFO order.
    [[nodiscard]] std::optional<Transaction> wait_pop(const std::stop_token& token);

    // Non-blocking variant; nullopt if empty (even if open).
    [[nodiscard]] std::optional<Transaction> try_pop();

    // Idempotent. Wakes all waiters; subsequent enqueue() fails;
    // wait_pop() drains the remainder, then returns nullopt.
    void close();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool closed() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::queue<Transaction> queue_;
    bool closed_{false};
};

} // namespace bank
