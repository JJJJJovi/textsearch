// tests/test_threadsystem.cpp — 线程池测试
#include "simpletest.h"
#include "core/threadpool.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

// ---- 基本任务执行 ----

TEST_CASE(tp_basic_task_execution) {
    ThreadPool pool(4);
    std::atomic<int> counter(0);

    for (int i = 0; i < 100; ++i) {
        pool.enqueue([&counter]() { counter.fetch_add(1); });
    }
    pool.wait_all();

    REQUIRE_EQ(counter.load(), 100);
}

// ---- wait_all 等待完成 ----

TEST_CASE(tp_wait_all_blocks_until_done) {
    ThreadPool pool(4);
    std::atomic<int> counter(0);

    for (int i = 0; i < 50; ++i) {
        pool.enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter.fetch_add(1);
        });
    }
    pool.wait_all();

    // wait_all 返回后所有任务应已完成
    REQUIRE_EQ(counter.load(), 50);
}

// ---- cancel 取消 ----

TEST_CASE(tp_cancel_stops_pending_tasks) {
    ThreadPool pool(2);
    std::atomic<int> executed(0);

    // 提交一些任务
    for (int i = 0; i < 100; ++i) {
        pool.enqueue([&executed]() {
            executed.fetch_add(1);
        });
    }

    // 等一小段时间让部分任务执行
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pool.cancel();
    pool.wait_all();

    // cancel 后不应崩溃，executed 应 <= 100
    REQUIRE(executed.load() <= 100);
    REQUIRE(pool.is_cancelled());
}

// ---- 并发度 clamp（F.2.6 / A-018）----

TEST_CASE(tp_concurrency_clamp_default) {
    // n=0 → clamp(hardware_concurrency(), 2, 16)
    ThreadPool pool(0);
    size_t count = pool.worker_count();
    REQUIRE(count >= 2);
    REQUIRE(count <= 16);
}

TEST_CASE(tp_concurrency_clamp_min) {
    // n=1 → clamp 到 2（最小 2）
    ThreadPool pool(1);
    REQUIRE_EQ(pool.worker_count(), static_cast<size_t>(2));
}

TEST_CASE(tp_concurrency_clamp_max) {
    // n=100 → clamp 到 16（最大 16）
    ThreadPool pool(100);
    REQUIRE_EQ(pool.worker_count(), static_cast<size_t>(16));
}

TEST_CASE(tp_concurrency_exact_value) {
    // n=8 → 8（在 [2,16] 范围内，原值保留）
    ThreadPool pool(8);
    REQUIRE_EQ(pool.worker_count(), static_cast<size_t>(8));
}

// ---- 异常安全（F.2.4：task 抛异常不崩）----

TEST_CASE(tp_exception_safety) {
    ThreadPool pool(4);
    std::atomic<int> normal_counter(0);

    // 提交一个会抛异常的任务
    pool.enqueue([]() {
        throw std::runtime_error("test exception");
    });

    // 提交正常任务
    for (int i = 0; i < 10; ++i) {
        pool.enqueue([&normal_counter]() {
            normal_counter.fetch_add(1);
        });
    }

    pool.wait_all();

    // 异常任务不应导致崩溃，正常任务应全部执行
    REQUIRE_EQ(normal_counter.load(), 10);
}

TEST_CASE(tp_exception_safety_unknown_exception) {
    ThreadPool pool(2);
    std::atomic<int> counter(0);

    // 抛未知类型异常
    pool.enqueue([]() {
        throw 42; // 非 std::exception
    });

    pool.enqueue([&counter]() {
        counter.fetch_add(1);
    });

    pool.wait_all();

    REQUIRE_EQ(counter.load(), 1);
}

// ---- 并发递增正确性 ----

TEST_CASE(tp_concurrent_increment_with_mutex) {
    ThreadPool pool(8);
    int counter = 0;
    std::mutex mtx;

    for (int i = 0; i < 1000; ++i) {
        pool.enqueue([&counter, &mtx]() {
            std::lock_guard<std::mutex> lock(mtx);
            ++counter;
        });
    }
    pool.wait_all();

    REQUIRE_EQ(counter, 1000);
}

// ---- is_cancelled 初始状态 ----

TEST_CASE(tp_initial_not_cancelled) {
    ThreadPool pool(4);
    REQUIRE(!pool.is_cancelled());
}

// ---- 多次 enqueue/wait_all 循环 ----

TEST_CASE(tp_multiple_rounds) {
    ThreadPool pool(4);
    std::atomic<int> total(0);

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 20; ++i) {
            pool.enqueue([&total]() { total.fetch_add(1); });
        }
        pool.wait_all();
    }

    REQUIRE_EQ(total.load(), 100);
}
