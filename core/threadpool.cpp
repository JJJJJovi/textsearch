// core/threadpool.cpp — 文件级并行线程池实现
#include "core/threadpool.h"

#include <algorithm>

// 并发度 clamp 范围（附录 F.2.6 / A-018）
static constexpr size_t MIN_THREADS = 2;
static constexpr size_t MAX_THREADS = 16;

// 计算 worker 线程数：0 → clamp(hardware_concurrency(), 2, 16)
static size_t resolve_thread_count(size_t n) {
    if (n == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        // hardware_concurrency 可能返回 0（无法探测），回退到 MIN_THREADS
        size_t base = (hw == 0) ? MIN_THREADS : static_cast<size_t>(hw);
        n = base;
    }
    return std::max(MIN_THREADS, std::min(n, MAX_THREADS));
}

ThreadPool::ThreadPool(size_t n)
    : cancelled_(false), active_count_(0), stop_(false) {
    size_t count = resolve_thread_count(n);
    workers_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    wait_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return stop_ || !tasks_.empty();
            });

            if (stop_ && tasks_.empty()) {
                return;
            }

            // 取消时：清空剩余任务，但仍执行当前已弹出的以保证
            // active_count 计数正确（实际上取消后队列被清空，这里取不到任务）
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                // 修复（2026-10）：必须在持锁期间登记活跃任务。旧实现在
                // 锁外递增——弹出最后一个任务后、递增前，wait_all 拿锁
                // 检查谓词 tasks_.empty() && active==0 会瞬时成立而提前
                // 返回（实测 300 轮丢 1 次）→ Scanner/Indexer 少一批结果，
                // 表现为偶发漏报。
                active_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                // stop_ 但 tasks_ 空 → 退出
                continue;
            }
        }

        // 执行任务（enqueue 已包装 try/catch，异常不会传播）
        task();

        active_count_.fetch_sub(1, std::memory_order_relaxed);

        // 检查是否全部完成，通知 wait_all
        if (active_count_.load(std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (tasks_.empty()) {
                wait_cv_.notify_all();
            }
        }
    }
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    wait_cv_.wait(lock, [this]() {
        // 等待条件：队列空 且 无活跃任务 或 已 stop
        return (tasks_.empty() &&
                active_count_.load(std::memory_order_relaxed) == 0) ||
               stop_;
    });
}

void ThreadPool::cancel() {
    // A-012：relaxed 序足够（取消是单向状态转换）
    cancelled_.store(true, std::memory_order_relaxed);
    // 清空待执行任务队列，让 wait_all 尽快返回
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::queue<std::function<void()>> empty;
        tasks_.swap(empty);
    }
    wait_cv_.notify_all();
}

bool ThreadPool::is_cancelled() const {
    return cancelled_.load(std::memory_order_relaxed);
}
