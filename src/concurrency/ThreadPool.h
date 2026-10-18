#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

namespace bank {

// Fixed-size pool of std::jthread workers.
// Tasks are std::function (must be copy-constructible; C++20 has no
// std::move_only_function). Non-copyable, non-movable.
class ThreadPool {
public:
    // thread_count == 0 is legal: submit() still queues, and stop() drains
    // the queue synchronously on the calling thread — useful for
    // deterministic single-threaded tests.
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();  // calls stop()

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Returns false once stop() has begun (no new work accepted).
    bool submit(std::function<void()> task);

    // Orderly shutdown: (1) flip stopping_ so submit() refuses new work,
    // (2) request_stop + notify_all, (3) workers drain the task queue
    // completely, then exit, (4) join all jthreads. With zero workers the
    // drain happens synchronously here.
    // Idempotent. Must NOT be called from a pool worker thread
    // (it would self-join; precondition, asserted in debug).
    void stop();

    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t pending() const;  // tasks queued, not yet started

private:
    void worker(std::stop_token token);

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    bool stopping_{false};
};

} // namespace bank
