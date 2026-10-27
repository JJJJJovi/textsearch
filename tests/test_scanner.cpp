// tests/test_scanner.cpp — 扫描器测试
#include "simpletest.h"
#include "core/scanner.h"
#include "core/filesystem.h"
#include "core/matcher.h"
#include "core/encoding.h"
#include "core/text_detect.h"
#include "core/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// ---- 测试辅助函数 ----

// 创建临时目录
static std::filesystem::path make_temp_dir(const std::wstring& name) {
    auto base = std::filesystem::temp_directory_path() / L"textsearch_test";
    auto path = base / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

// 写入文本文件（UTF-8）
static std::filesystem::path write_text_file(
    const std::filesystem::path& dir,
    const std::wstring& name,
    const std::wstring& content) {
    auto path = dir / name;
    std::string utf8 = wstring_to_utf8(content);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return path;
}

// 写入字节文件
static std::filesystem::path write_bytes_file(
    const std::filesystem::path& dir,
    const std::wstring& name,
    const std::vector<unsigned char>& data) {
    auto path = dir / name;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return path;
}

// 串行扫描辅助：用 T1-T3 API 直接扫描，作为"正确基线"
static std::vector<MatchResult> serial_scan(
    const std::filesystem::path& root,
    const std::set<std::wstring>& ext_set,
    const SearchOptions& opts) {
    std::vector<MatchResult> results;
    auto files = enumerate_files(root, ext_set, opts.recursive);

    int flags = MatchFlags::Substring;
    if (opts.use_regex) flags |= MatchFlags::Regex;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    for (const auto& path : files) {
        // mtime 过滤
        if (opts.mtime_filter.has_value()) {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (!ec) {
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                time_t mtime = std::chrono::system_clock::to_time_t(sctp);
                auto [from, to] = opts.mtime_filter.value();
                if (mtime < from || mtime > to) continue;
            }
        }

        // 二进制跳过
        auto head = read_head_bytes(path, 8192);
        if (!is_textual(head)) continue;

        std::wstring text = read_text(path);
        auto hits = search_in_text(text, opts.pattern, flags, path, opts.max_results);
        for (auto& h : hits) {
            results.push_back(std::move(h));
            if (results.size() >= opts.max_results) return results;
        }
    }
    return results;
}

// 收集 Scanner 结果的辅助
struct ScanCollector {
    std::mutex mtx;
    std::vector<MatchResult> all_results;
    std::atomic<size_t> last_done{0};
    std::atomic<size_t> last_total{0};
    std::atomic<size_t> skip_count{0};
    std::vector<std::wstring> errors;
    std::atomic<bool> completed{false};

    ScanCallbacks make_callbacks() {
        ScanCallbacks cb;
        cb.on_progress = [this](size_t done, size_t total) {
            last_done.store(done);
            last_total.store(total);
        };
        cb.on_results = [this](std::vector<MatchResult> r) {
            std::lock_guard<std::mutex> lock(mtx);
            for (auto& m : r) {
                all_results.push_back(std::move(m));
            }
        };
        cb.on_error = [this](const std::wstring& msg) {
            std::lock_guard<std::mutex> lock(mtx);
            errors.push_back(msg);
        };
        cb.on_skip = [this](size_t skipped) {
            skip_count.store(skipped);
        };
        cb.on_complete = [this]() {
            completed.store(true);
        };
        return cb;
    }
};

// 结果排序辅助（用于比较，忽略顺序）
static std::vector<MatchResult> sort_results(std::vector<MatchResult> r) {
    std::sort(r.begin(), r.end(), [](const MatchResult& a, const MatchResult& b) {
        if (a.file_path != b.file_path) return a.file_path < b.file_path;
        if (a.line_no != b.line_no) return a.line_no < b.line_no;
        return a.col < b.col;
    });
    return r;
}

// ---- 即时扫描基本流程 ----

TEST_CASE(scan_realtime_basic) {
    auto dir = make_temp_dir(L"scan_basic");
    write_text_file(dir, L"a.txt", L"hello world\nfoo bar\nhello again");
    write_text_file(dir, L"b.txt", L"no match here\nhello there");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"hello";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();

    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(3));
    REQUIRE_EQ(col.last_total.load(), static_cast<size_t>(2));
    REQUIRE_EQ(col.last_done.load(), static_cast<size_t>(2));

    std::filesystem::remove_all(dir);
}

// ---- 串行 vs 并行结果一致（T4 验收要求）----

TEST_CASE(scan_serial_vs_parallel_consistent) {
    auto dir = make_temp_dir(L"scan_consistency");
    // 多文件多内容
    for (int i = 0; i < 10; ++i) {
        std::wstring content = L"line search_me here\nanother line\nsearch_me again\n";
        write_text_file(dir, L"f" + std::to_wstring(i) + L".txt", content);
    }

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"search_me";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    // 串行基线
    auto serial = serial_scan(dir, ext_set, opts);

    // 并行扫描
    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE_EQ(col.all_results.size(), serial.size());

    auto sorted_parallel = sort_results(col.all_results);
    auto sorted_serial = sort_results(serial);

    REQUIRE_EQ(sorted_parallel.size(), sorted_serial.size());
    for (size_t i = 0; i < sorted_parallel.size(); ++i) {
        REQUIRE_EQ(sorted_parallel[i].line_no, sorted_serial[i].line_no);
        REQUIRE_EQ(sorted_parallel[i].col, sorted_serial[i].col);
    }

    std::filesystem::remove_all(dir);
}

// ---- 串行 vs 并行：正则模式一致 ----

TEST_CASE(scan_serial_vs_parallel_regex) {
    auto dir = make_temp_dir(L"scan_regex_consistency");
    write_text_file(dir, L"a.txt", L"2026-08-26 date\n2026-01-15 date\nno date here");
    write_text_file(dir, L"b.txt", L"2025-12-31 end\nrandom text");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"\\d{4}-\\d{2}-\\d{2}";
    opts.use_regex = true;
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    auto serial = serial_scan(dir, ext_set, opts);

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE_EQ(col.all_results.size(), serial.size());

    std::filesystem::remove_all(dir);
}

// ---- 取消中途生效 ----

TEST_CASE(scan_cancel_midway) {
    auto dir = make_temp_dir(L"scan_cancel");
    // 大量文件
    for (int i = 0; i < 200; ++i) {
        write_text_file(dir, L"f" + std::to_wstring(i) + L".txt", L"search_target content");
    }

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"search_target";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();

    Scanner scanner;
    // 在另一个线程中启动扫描
    std::thread scan_thread([&]() {
        scanner.scan_realtime(opts, ext_set, cb);
    });

    // 等一小段时间后取消
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scanner.cancel();

    scan_thread.join();

    REQUIRE(col.completed.load());
    // 取消后结果数应 < 总文件数（可能部分已扫描完）
    // 关键是不崩溃且完成回调被触发
    REQUIRE(col.all_results.size() <= 200);

    std::filesystem::remove_all(dir);
}

// ---- 跳过二进制文件 ----

TEST_CASE(scan_skip_binary_files) {
    auto dir = make_temp_dir(L"scan_binary");
    write_text_file(dir, L"normal.txt", L"findme in text");

    // 构造含 NUL 的二进制文件
    std::vector<unsigned char> bin_data = {'f', 'i', 'n', 'd', 'm', 'e',
                                           0x00, 0x01, 0x02, 0x03, 'x', 'y'};
    write_bytes_file(dir, L"binary.txt", bin_data);

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"findme";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    // 只应命中 normal.txt，binary.txt 被跳过
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(1));
    REQUIRE(col.skip_count.load() >= 1); // 至少跳过 1 个二进制

    std::filesystem::remove_all(dir);
}

// ---- 大文件流式（构造 >阈值 的文件，验证命中正确）----

TEST_CASE(scan_bigfile_streaming_correct) {
    auto dir = make_temp_dir(L"scan_bigfile");

    // 设置阈值为 1MB，创建 2MB 文件
    // 构造内容：重复的行，每 N 行插入一次 pattern
    std::wstring content;
    std::wstring line = L"padding line content for testing big file streaming scan\n";
    // 构造约 2MB 内容
    while (content.size() < 2 * 1024 * 1024) {
        content += L"normal padding line\n";
        // 每 100 行插入一个 pattern 行
        content += L"TARGET_PATTERN line\n";
    }

    auto path = write_text_file(dir, L"big.txt", content);

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"TARGET_PATTERN";
    opts.bigfile_threshold_mb = 1; // 1MB 阈值，2MB 文件走 MMF
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    // 串行基线（串行用 read_text 全量读，不走 MMF）
    auto serial = serial_scan(dir, ext_set, opts);

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    // 并行（MMF）结果数应与串行一致
    REQUIRE_EQ(col.all_results.size(), serial.size());
    // 应有多个命中
    REQUIRE(col.all_results.size() > 0);

    std::filesystem::remove_all(dir);
}

// ---- 空路径报错 ----

TEST_CASE(scan_empty_pattern_error) {
    auto dir = make_temp_dir(L"scan_empty_pattern");
    write_text_file(dir, L"a.txt", L"hello world");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L""; // 空 pattern
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE(!col.errors.empty()); // 应有错误提示
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(0));

    std::filesystem::remove_all(dir);
}

// ---- 不存在路径 ----

TEST_CASE(scan_nonexistent_path) {
    SearchOptions opts;
    opts.root = L"C:\\nonexistent_path_12345_abcde";
    opts.pattern = L"test";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE(!col.errors.empty()); // 应有错误提示

    // 清理（目录不存在，remove_all 安全）
    std::error_code ec;
    std::filesystem::remove_all(opts.root, ec);
}

// ---- 修改时间过滤 ----

TEST_CASE(scan_mtime_filter_include) {
    auto dir = make_temp_dir(L"scan_mtime_include");
    write_text_file(dir, L"recent.txt", L"target line here");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"target";
    opts.recursive = true;

    // 设置时间范围为 [now-3600, now+3600]，应包含刚创建的文件
    time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    opts.mtime_filter = std::make_pair(now - 3600, now + 3600);

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(1)); // 应命中

    std::filesystem::remove_all(dir);
}

TEST_CASE(scan_mtime_filter_exclude) {
    auto dir = make_temp_dir(L"scan_mtime_exclude");
    write_text_file(dir, L"recent.txt", L"target line here");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"target";
    opts.recursive = true;

    // 设置时间范围为未来 [now+3600, now+7200]，应排除刚创建的文件
    time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    opts.mtime_filter = std::make_pair(now + 3600, now + 7200);

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(0)); // 应被过滤

    std::filesystem::remove_all(dir);
}

// ---- 结果上限 max_results ----

TEST_CASE(scan_max_results_cap) {
    auto dir = make_temp_dir(L"scan_max_results");
    // 创建多个文件，每个含多个命中
    for (int i = 0; i < 10; ++i) {
        write_text_file(dir, L"f" + std::to_wstring(i) + L".txt",
                        L"hit hit hit hit hit");
    }

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"hit";
    opts.recursive = true;
    opts.max_results = 20; // 上限 20

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    // 结果数应 <= max_results
    REQUIRE(col.all_results.size() <= 20);

    std::filesystem::remove_all(dir);
}

// ---- 递归子目录 ----

TEST_CASE(scan_recursive_subdirs) {
    auto dir = make_temp_dir(L"scan_recursive");
    auto subdir = dir / L"sub1" / L"sub2";
    std::error_code ec;
    std::filesystem::create_directories(subdir, ec);

    write_text_file(dir, L"top.txt", L"findme at top");
    write_text_file(dir / L"sub1", L"mid.txt", L"findme at mid");
    write_text_file(subdir, L"deep.txt", L"findme at deep");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"findme";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(3));

    std::filesystem::remove_all(dir);
}

// ---- 非递归 ----

TEST_CASE(scan_non_recursive) {
    auto dir = make_temp_dir(L"scan_nonrecursive");
    auto subdir = dir / L"sub";
    std::error_code ec;
    std::filesystem::create_directories(subdir, ec);

    write_text_file(dir, L"top.txt", L"findme at top");
    write_text_file(subdir, L"deep.txt", L"findme at deep");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"findme";
    opts.recursive = false; // 非递归

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(1)); // 只扫顶层

    std::filesystem::remove_all(dir);
}

// ---- 无效正则报错 ----

TEST_CASE(scan_invalid_regex_error) {
    auto dir = make_temp_dir(L"scan_invalid_regex");
    write_text_file(dir, L"a.txt", L"some text");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"[invalid"; // 无效正则
    opts.use_regex = true;
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE(!col.errors.empty()); // 应有正则错误提示
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(0));

    std::filesystem::remove_all(dir);
}

// ---- 忽略大小写 ----

TEST_CASE(scan_ignore_case) {
    auto dir = make_temp_dir(L"scan_ignorecase");
    write_text_file(dir, L"a.txt", L"Hello HELLO hello");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"hello";
    opts.ignore_case = true;
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;
    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(col.completed.load());
    REQUIRE_EQ(col.all_results.size(), static_cast<size_t>(3));

    std::filesystem::remove_all(dir);
}

// ---- is_running 状态 ----

TEST_CASE(scan_is_running_state) {
    auto dir = make_temp_dir(L"scan_running");
    write_text_file(dir, L"a.txt", L"hello");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"hello";
    opts.recursive = true;

    std::set<std::wstring> ext_set = {L".txt"};

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner scanner;

    REQUIRE(!scanner.is_running()); // 初始不运行

    scanner.scan_realtime(opts, ext_set, cb);

    REQUIRE(!scanner.is_running()); // 完成后不运行

    std::filesystem::remove_all(dir);
}

// ============================================================
// S5.3：超长行重叠切片去重（切片阈值注入小常量）
// ============================================================

namespace {
struct MaxLineGuard {
    size_t saved;
    explicit MaxLineGuard(size_t v) : saved(get_max_line_bytes()) {
        set_max_line_bytes(v);
    }
    ~MaxLineGuard() { set_max_line_bytes(saved); }
};
}

TEST_CASE(scanner_longline_overlap_dedup) {
    auto dir = make_temp_dir(L"scan_longline_dedup");
    // 82 字符单行："ab"×40 + "zz"。切片 16/overlap 4 下，切片边界附近
    // 同一匹配位置会被相邻两片各报一次（短匹配 ≤ overlap）→ 旧实现重复。
    std::wstring line;
    for (int i = 0; i < 40; ++i) line += L"ab";
    line += L"zz";
    write_text_file(dir, L"big.txt", line + L"\n" + line + L"\n");

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"ab|zz";
    opts.use_regex = true;
    opts.recursive = true;
    opts.force_realtime = true;
    opts.bigfile_threshold_mb = 0;   // 全部走大文件流式路径（切片逻辑所在）
    std::set<std::wstring> ext_set = {L".txt"};

    MaxLineGuard guard(16);   // 切片阈值 = 16 字符

    ScanCollector col;
    auto cb = col.make_callbacks();
    Scanner sc;
    sc.scan_realtime(opts, ext_set, cb);

    std::vector<std::pair<int, int>> got;
    {
        std::lock_guard<std::mutex> lock(col.mtx);
        for (auto& m : col.all_results) got.push_back({m.line_no, m.col});
    }
    std::sort(got.begin(), got.end());
    for (size_t i = 1; i < got.size(); ++i) {
        REQUIRE(!(got[i] == got[i - 1]));   // 无重复
    }

    // 与串行基线（整行匹配，不切片）逐条一致：去重同时不漏报
    auto expect = serial_scan(dir, ext_set, opts);
    std::vector<std::pair<int, int>> want;
    for (auto& m : expect) want.push_back({m.line_no, m.col});
    std::sort(want.begin(), want.end());
    REQUIRE_EQ(got.size(), want.size());
    for (size_t i = 0; i < want.size(); ++i) REQUIRE(got[i] == want[i]);
    REQUIRE_EQ(got.size(), static_cast<size_t>(82));   // 2 行 × 41 处

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================
// S3.2：Scanner::cancel 与 pool_ 创建/reset 的竞争冒烟
// cancel()（模拟 GUI 主线程）可能撞上 worker 线程 make_unique&lt;ThreadPool&gt;
// /reset 的瞬间 → 旧实现读半构造 pool_ 属未定义行为。50 轮无崩溃即通过。
// ============================================================

TEST_CASE(scanner_cancel_race_smoke) {
    auto dir = make_temp_dir(L"scan_cancel_race");
    for (int i = 0; i < 30; ++i) {
        write_text_file(dir, L"f" + std::to_wstring(i) + L".txt",
                        L"alpha beta needle line\nneedle two\n");
    }

    SearchOptions opts;
    opts.root = dir;
    opts.pattern = L"needle";
    opts.recursive = true;
    opts.force_realtime = true;
    std::set<std::wstring> ext_set = {L".txt"};

    for (int iter = 0; iter < 50; ++iter) {
        auto sc = std::make_shared<Scanner>();
        ScanCollector col;
        auto cb = col.make_callbacks();
        std::thread t([sc, opts, ext_set, &cb]() mutable {
            sc->scan_realtime(opts, ext_set, cb);
        });
        // 变化时序：分别瞄准 pool_ 创建前 / 创建中 / reset 附近
        if (iter % 3 == 1)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if (iter % 3 == 2)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        sc->cancel();
        t.join();
    }
    // 无崩溃/死锁走到这里即通过

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
