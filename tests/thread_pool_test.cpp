#include "thread_pool.h"
#include "task_gate.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <stdexcept>

using namespace chatroom;
using namespace std::chrono_literals;

TEST_CASE("A fixed pool runs two tasks while a third waits", "[thread_pool]") {
    std::promise<void> first_started;
    std::promise<void> second_started;
    std::promise<void> third_started;
    auto first = first_started.get_future();
    auto second = second_started.get_future();
    auto third = third_started.get_future();
    ThreadPool pool(2);
    TaskGate gate;
    pool.submit([&, wait = gate.future] {
        first_started.set_value();
        wait.wait();
    });
    pool.submit([&, wait = gate.future] {
        second_started.set_value();
        wait.wait();
    });
    REQUIRE(first.wait_for(2s) == std::future_status::ready);
    REQUIRE(second.wait_for(2s) == std::future_status::ready);
    pool.submit([&] { third_started.set_value(); });
    CHECK(third.wait_for(0s) == std::future_status::timeout);
    gate.release();
    REQUIRE(third.wait_for(2s) == std::future_status::ready);
}

TEST_CASE("Pool shutdown drains accepted tasks and rejects new submissions", "[thread_pool]") {
    std::atomic<int> completed{0};
    ThreadPool pool(1);
    for (int i = 0; i < 10; ++i) {
        pool.submit([&] { ++completed; });
    }
    pool.shutdown();
    CHECK(completed == 10);
    CHECK_THROWS_AS(pool.submit([] {}), std::runtime_error);
}

TEST_CASE("A throwing task does not remove a worker", "[thread_pool]") {
    std::promise<void> completed;
    auto ready = completed.get_future();
    ThreadPool pool(1);
    pool.submit([] { throw std::runtime_error("Expected test failure"); });
    pool.submit([&] { completed.set_value(); });
    REQUIRE(ready.wait_for(2s) == std::future_status::ready);
}
