// core/threadpool.h — 文件级并行线程池
// 附录 F.2.4 Worker 异常处理 / F.2.6 内存约束（并发度 clamp）/ A-018
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

// 固定大小线程池，支持任务提交、等待全部完成、协作式取消。
// 设计要点：
//   - 并发度 clamp(hardware_concurrency(), 2, 16)（附录 F.2.6 / A-018）
//   - F.2.4：enqueue 内部包装 task 为 try/catch，异常不传播到 worker 线程外
//   - cancelled 用 atomic<bool>，memory_order_relaxed（A-012）
//   - wait_all 等待所有任务完成或取消
//   - 析构时 stop_=true，通知所有 worker 退出
class ThreadPool {
public:
    // n == 0 → clamp(hardware_concurrency(), 2, 16)（F.2.6 / A-018）
    explicit ThreadPool(size_t n = 0);

    // 析构：置 stop_ 并唤醒所有 worker，join 全部线程
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务（F.2.4：内部包装 try/catch，异常不传播）
    template <class F>
    void enqueue(F&& f) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            tasks_.emplace([task = std::forward<F>(f)]() {
                // F.2.4：worker 入口 try/catch，异常被吞掉不传播到线程外
                try {
                    task();
                } catch (...) {
                    // 异常吞掉；上层 Scanner 已在每个文件粒度
                    // 自行 try/catch 并通过 on_error 回调报告，
                    // 此处是最后一道防线，防止异常逃逸到 worker 线程。
                }
            });
        }
        cv_.notify_one();
    }

    // 等待所有已提交任务完成（或取消）
    void wait_all();

    // 设置取消标志（协作式：worker 每文件检查）
    void cancel();

    // 是否已取消
    bool is_cancelled() const;

    // worker 线程数
    size_t worker_count() const { return workers_.size(); }

private:
    // worker 线程主循环
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;        // 任务到达通知
    std::condition_variable wait_cv_;   // 任务全部完成通知
    std::atomic<bool> cancelled_{false};
    std::atomic<size_t> active_count_{0}; // 正在执行的任务数
    bool stop_{false};
};
