#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace chatroom {

// Fixed workers sharing a FIFO task queue. Tasks run without holding the queue lock.
class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(std::size_t thread_count = 4);
    ~ThreadPool();

    // Rejects submissions after shutdown starts.
    void submit(Task task);

    // Drains accepted tasks and joins workers. Call from the owner, not a worker.
    void shutdown();

private:
    void work();

    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Task> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace chatroom
