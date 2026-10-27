// core/scanner.h — 扫描引擎（即时模式 T4a + 索引模式 T4b 调度）
// 设计原则：Scanner 与 GUI 解耦——用回调接口而非直接 PostMessage，便于单元测试。
#pragma once

#include "core/types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// 扫描回调接口（GUI 层负责包装 PostMessage）
// 所有回调由 Scanner 在 worker 线程中调用，GUI 层需自行 PostMessage 回主线程。
struct ScanCallbacks {
    // 进度回调（done 已处理，total 总数）
    std::function<void(size_t done, size_t total)> on_progress;

    // 结果批次回调（积累到 100 条或文件处理完毕时触发，F.2.9 批次聚合）
    std::function<void(std::vector<MatchResult>)> on_results;

    // 错误/警告回调（F.2.4：worker 异常通过此回调报告）
    std::function<void(const std::wstring& msg)> on_error;

    // 跳过统计回调（F.2.7：无权限/锁定/二进制文件）
    std::function<void(size_t skipped)> on_skip;

    // 完成回调（扫描结束，无论完成/取消/错误）
    std::function<void()> on_complete;
};

// 扫描器：即时扫描 + 索引搜索调度
class Scanner {
public:
    Scanner();
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // 即时扫描（T4a）
    // - opts:     搜索选项
    // - ext_set:  已解析的最终扩展名集合
    // - callbacks: 回调接口
    void scan_realtime(const SearchOptions& opts,
                       const std::set<std::wstring>& ext_set,
                       ScanCallbacks& callbacks);

    // 取消（协作式：worker 每文件检查）
    void cancel();

    // 是否正在扫描
    bool is_running() const;

private:
    // 处理单个文件（线程池任务）
    void process_file(const std::filesystem::path& path,
                      const SearchOptions& opts,
                      size_t& global_done,
                      size_t& global_skipped,
                      std::vector<MatchResult>& batch,
                      std::mutex& batch_mutex,
                      std::mutex& counter_mutex,
                      const ScanCallbacks& callbacks,
                      size_t total,
                      size_t& total_results,
                      bool& max_reached);

    // 小文件全量读取匹配
    std::vector<MatchResult> scan_small_file(
        const std::filesystem::path& path,
        const SearchOptions& opts);

    // 大文件内存映射流式匹配（F.2.6：64MB 窗口滑动）
    std::vector<MatchResult> scan_big_file(
        const std::filesystem::path& path,
        const SearchOptions& opts);

    // 刷新批次结果到回调
    void flush_batch(std::vector<MatchResult>& batch,
                     std::mutex& batch_mutex,
                     const ScanCallbacks& callbacks);

    // pool_ 生命周期锁：scan worker 创建/reset 与主线程 cancel() 中
    // pool_->cancel() 互斥（cancel 可能读到半构造/已释放的 ThreadPool*）
    std::mutex pool_mutex_;
    std::unique_ptr<class ThreadPool> pool_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    // 传给 matcher 的取消标志（正则模式逐行中断检查）
    const std::atomic<bool>* regex_cancel_ptr_ = nullptr;
};

// S5.3：超长行切片阈值（默认 16M 字符）。仅测试注入小常量用；
// 非线程安全，请在无扫描运行时设置，用完恢复。
void set_max_line_bytes(size_t n);
size_t get_max_line_bytes();
