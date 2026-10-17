#include "thread_pool.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace chatroom {

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        throw std::invalid_argument("Thread pool requires at least one worker");
    }
    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    } catch (...) {
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::submit(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            throw std::runtime_error("Thread pool is shutting down");
        }
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
}

void ThreadPool::work() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception& error) {
            std::cerr << "Worker task failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "Worker task failed with an unknown exception\n";
        }
    }
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace chatroom
