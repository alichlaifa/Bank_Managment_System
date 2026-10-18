#include "concurrency/TransactionQueue.h"

namespace bank {

bool TransactionQueue::enqueue(Transaction txn)
{
    {
        std::lock_guard<std::mutex> guard{mutex_};
        if (closed_) {
            return false;
        }
        queue_.push(std::move(txn));
    }
    cv_.notify_one();
    return true;
}

std::optional<Transaction> TransactionQueue::wait_pop(const std::stop_token& token)
{
    std::unique_lock<std::mutex> lock{mutex_};
    // The predicate is re-checked after a stop-token wakeup, so a stopped
    // token on a non-empty queue still drains rather than dropping items.
    cv_.wait(lock, token, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
        return std::nullopt;  // closed (or stopped) and drained
    }
    Transaction txn = std::move(queue_.front());
    queue_.pop();
    return txn;
}

std::optional<Transaction> TransactionQueue::try_pop()
{
    std::lock_guard<std::mutex> guard{mutex_};
    if (queue_.empty()) {
        return std::nullopt;
    }
    Transaction txn = std::move(queue_.front());
    queue_.pop();
    return txn;
}

void TransactionQueue::close()
{
    {
        std::lock_guard<std::mutex> guard{mutex_};
        closed_ = true;
    }
    cv_.notify_all();
}

std::size_t TransactionQueue::size() const
{
    std::lock_guard<std::mutex> guard{mutex_};
    return queue_.size();
}

bool TransactionQueue::closed() const
{
    std::lock_guard<std::mutex> guard{mutex_};
    return closed_;
}

} // namespace bank
