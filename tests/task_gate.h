#pragma once

#include <future>

// Releases blocked tasks during unwinding too. Tasks capture a copy of future.
struct TaskGate {
    std::promise<void> signal;
    std::shared_future<void> future = signal.get_future().share();

    ~TaskGate() { release(); }

    void release() {
        if (!released) {
            signal.set_value();
            released = true;
        }
    }

private:
    bool released = false;
};
