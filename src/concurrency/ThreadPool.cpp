#include "concurrency/ThreadPool.h"

#include <cassert>

namespace bank {

ThreadPool::ThreadPool(std::size_t thread_count)
{
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this](std::stop_token token) { worker(std::move(token)); });
    }
}

ThreadPool::~ThreadPool()
{
    stop();
}

bool ThreadPool::submit(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> guard{mutex_};
        if (stopping_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::stop()
{
#ifndef NDEBUG
    // May not be called from a pool worker: it would join itself.
    for (auto& thread : workers_) {
        assert(!thread.joinable() || thread.get_id() != std::this_thread::get_id());
    }
#endif

    {
        std::lock_guard<std::mutex> guard{mutex_};
        if (!stopping_) {
            stopping_ = true;
        }
    }
    cv_.notify_all();

    for (auto& thread : workers_) {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }
    workers_.clear();

    // Zero-worker pool (or belt-and-braces after joins): drain any remaining
    // tasks synchronously on the calling thread so stop() never drops work.
    while (true) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> guard{mutex_};
            if (tasks_.empty()) {
                break;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

std::size_t ThreadPool::pending() const
{
    std::lock_guard<std::mutex> guard{mutex_};
    return tasks_.size();
}

void ThreadPool::worker(std::stop_token token)
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            cv_.wait(lock, token, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_) {
                    return;  // stop requested and queue fully drained
                }
                continue;  // spurious wake
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

} // namespace bank
