// tests/bench_search.cpp — 性能 Benchmark（F.1.5 / A-002）
// 对照附录 F.1.5 基线指标，测量即时扫描/索引建库/索引查询/大文件流式扫描的耗时。
// 输出 JSON 报告到 stdout，不断言结果。
#include "core/indexer.h"
#include "core/scanner.h"
#include "core/encoding.h"
#include "core/filesystem.h"
#include "core/matcher.h"
#include "core/types.h"
#include "core/util.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

// ---- 计时辅助 ----
using Clock = std::chrono::high_resolution_clock;

static double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ---- 内存峰值辅助（Windows API）----
static size_t get_process_memory_mb() {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.PeakWorkingSetSize / (1024 * 1024);
    }
    return 0;
}

// ---- 写 UTF-8 文件 ----
static void write_utf8_file(const std::filesystem::path& path,
                            const std::wstring& content) {
    std::string utf8 = wstring_to_utf8(content);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// ---- 写 GBK 文件 ----
static void write_gbk_file(const std::filesystem::path& path,
                           const std::wstring& content) {
    if (content.empty()) {
        std::ofstream ofs(path, std::ios::binary);
        return;
    }
    int len = WideCharToMultiByte(936, 0,
        content.c_str(), static_cast<int>(content.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) { std::ofstream ofs(path, std::ios::binary); return; }
    std::string gbk(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(936, 0,
        content.c_str(), static_cast<int>(content.size()),
        gbk.data(), len, nullptr, nullptr);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(gbk.data(), static_cast<std::streamsize>(gbk.size()));
}

// ---- 创建测试目录（~100 个小文件，混合编码）----
static std::filesystem::path create_test_corpus() {
    auto base = std::filesystem::temp_directory_path() / L"textsearch_bench";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);

    // 模拟代码库内容（中英混合）
    const std::wstring code_lines[] = {
        L"#include <stdio.h>\n",
        L"// 这是中文注释行\n",
        L"int main() {\n",
        L"    printf(\"hello world\\n\");\n",
        L"    // 返回值检查\n",
        L"    if (result != 0) {\n",
        L"        log_error(\"搜索失败\");\n",
        L"        return -1;\n",
        L"    }\n",
        L"    return 0;\n",
        L"}\n",
        L"// 中文文本搜索测试\n",
        L"const char* msg = \"全文查找工具\";\n",
        L"// 函数定义\n",
        L"void search_text(const char* pattern) {\n",
        L"    // 搜索中文内容\n",
        L"    printf(\"搜索: %s\\n\", pattern);\n",
        L"}\n",
    };
    const int num_code_lines = sizeof(code_lines) / sizeof(code_lines[0]);

    for (int i = 0; i < 80; ++i) {
        std::wstring content;
        for (int j = 0; j < 5; ++j) {
            content += code_lines[(i + j) % num_code_lines];
        }
        // 偶数文件用 UTF-8，奇数用 GBK
        if (i % 2 == 0) {
            write_utf8_file(base / (L"file_" + std::to_wstring(i) + L".txt"), content);
        } else {
            write_gbk_file(base / (L"file_" + std::to_wstring(i) + L".txt"), content);
        }
    }

    // 额外几个 .c 文件
    for (int i = 0; i < 20; ++i) {
        std::wstring content = L"// code file " + std::to_wstring(i) + L"\n"
            L"void func_" + std::to_wstring(i) + L"() {\n"
            L"    // 中文搜索内容\n"
            L"    int val = " + std::to_wstring(i * 100) + L";\n"
            L"}\n";
        write_utf8_file(base / (L"code_" + std::to_wstring(i) + L".c"), content);
    }

    return base;
}

// ---- 同步即时扫描（收集结果）----
static std::vector<MatchResult> realtime_search(
    const std::filesystem::path& root,
    const std::wstring& pattern,
    const std::set<std::wstring>& ext_set,
    bool use_regex = false) {
    SearchOptions opts;
    opts.root = root;
    opts.pattern = pattern;
    opts.recursive = true;
    opts.use_regex = use_regex;
    opts.force_realtime = true;

    std::vector<MatchResult> all;
    Scanner sc;
    ScanCallbacks cb;
    cb.on_results = [&](std::vector<MatchResult> r) {
        for (auto& m : r) all.push_back(std::move(m));
    };
    cb.on_complete = [&]() {};
    sc.scan_realtime(opts, ext_set, cb);
    return all;
}

// ---- 创建大文件（>50MB）----
static std::filesystem::path create_bigfile() {
    auto path = std::filesystem::temp_directory_path() / L"textsearch_bigfile.txt";
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return {};

    // 写约 55MB 内容：每行约 100 字节，重复 ~550000 行
    const std::string line =
        "2026-01-15 12:30:45 [INFO] This is a test log line for benchmarking "
        "the streaming search. keyword_to_find embedded here. Line ends.\n";
    const size_t target_size = 55ULL * 1024 * 1024;
    size_t written = 0;
    while (written < target_size) {
        ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
        written += line.size();
    }
    return path;
}

// ---- 主函数 ----
int main() {
    std::cout << "{\n";

    // === 1. 创建测试语料 ===
    std::cout << "  \"benchmark\": {\n";
    std::cout << "    \"baseline\": {\n";
    std::cout << "      \"instant_first_result_ms\": \"<1000\",\n";
    std::cout << "      \"instant_full_scan_ms\": \"<5000\",\n";
    std::cout << "      \"index_build_ms\": \"<10000\",\n";
    std::cout << "      \"index_query_ms\": \"<200\",\n";
    std::cout << "      \"bigfile_memory_peak_mb\": \"<100\",\n";
    std::cout << "      \"instant_peak_memory_mb\": \"<512\"\n";
    std::cout << "    },\n";

    auto corpus = create_test_corpus();
    std::set<std::wstring> ext_set = {L".txt", L".c"};

    // === 2. 即时模式全量搜索 ===
    {
        auto t0 = Clock::now();
        auto results = realtime_search(corpus, L"搜索", ext_set);
        auto t1 = Clock::now();
        double ms = elapsed_ms(t0, t1);
        size_t mem = get_process_memory_mb();

        std::cout << "    \"instant_scan\": {\n";
        std::cout << "      \"pattern\": \"搜索\",\n";
        std::cout << "      \"files_scanned\": 100,\n";
        std::cout << "      \"results\": " << results.size() << ",\n";
        std::cout << "      \"elapsed_ms\": " << std::fixed
                  << std::setprecision(2) << ms << ",\n";
        std::cout << "      \"peak_memory_mb\": " << mem << "\n";
        std::cout << "    },\n";
    }

    // === 3. 索引模式建库 ===
    {
        auto t0 = Clock::now();
        Indexer idx;
        bool ok = idx.build(corpus, ext_set);
        auto t1 = Clock::now();
        double ms = elapsed_ms(t0, t1);

        std::cout << "    \"index_build\": {\n";
        std::cout << "      \"success\": " << (ok ? "true" : "false") << ",\n";
        std::cout << "      \"elapsed_ms\": " << std::fixed
                  << std::setprecision(2) << ms << ",\n";
        std::cout << "      \"index_exists\": "
                  << (Indexer::index_exists(corpus) ? "true" : "false") << "\n";
        std::cout << "    },\n";

        // === 4. 索引模式查询 ===
        if (ok) {
            Indexer idx2;
            idx2.open(corpus);

            // 子串查询
            {
                SearchOptions opts;
                opts.root = corpus;
                opts.pattern = L"搜索";
                opts.recursive = true;
                auto t0q = Clock::now();
                auto qresults = idx2.search(L"搜索", opts);
                auto t1q = Clock::now();
                double qms = elapsed_ms(t0q, t1q);

                std::cout << "    \"index_query_substring\": {\n";
                std::cout << "      \"pattern\": \"搜索\",\n";
                std::cout << "      \"results\": " << qresults.size() << ",\n";
                std::cout << "      \"elapsed_ms\": " << std::fixed
                          << std::setprecision(2) << qms << "\n";
                std::cout << "    },\n";
            }

            // 正则查询
            {
                SearchOptions opts;
                opts.root = corpus;
                opts.pattern = L"func_\\d+";
                opts.recursive = true;
                opts.use_regex = true;
                auto t0q = Clock::now();
                auto qresults = idx2.search(L"func_\\d+", opts);
                auto t1q = Clock::now();
                double qms = elapsed_ms(t0q, t1q);

                std::cout << "    \"index_query_regex\": {\n";
                std::cout << "      \"pattern\": \"func_\\\\d+\",\n";
                std::cout << "      \"results\": " << qresults.size() << ",\n";
                std::cout << "      \"elapsed_ms\": " << std::fixed
                          << std::setprecision(2) << qms << "\n";
                std::cout << "    },\n";
            }
        }
    }

    // === 5. 大文件流式扫描 ===
    {
        auto bigfile = create_bigfile();
        if (!bigfile.empty()) {
            SearchOptions opts;
            opts.root = bigfile.parent_path();
            opts.pattern = L"keyword_to_find";
            opts.recursive = true;
            opts.force_realtime = true;
            opts.bigfile_threshold_mb = 50;

            std::vector<MatchResult> all;
            Scanner sc;
            ScanCallbacks cb;
            cb.on_results = [&](std::vector<MatchResult> r) {
                for (auto& m : r) all.push_back(std::move(m));
            };
            cb.on_complete = [&]() {};

            // 需要只扫这一个文件
            std::set<std::wstring> big_ext = {L".txt"};

            auto t0 = Clock::now();
            // 直接用 scan_realtime 但限制到大文件目录
            // 由于大文件在 temp 目录，我们设置 root 为大文件路径的父目录
            // 但为了只扫这一个文件，我们直接用 scan_small_file 不可行
            // 用 scan_realtime 扫描整个 temp 目录会太慢
            // 改为：直接读取大文件并匹配
            // 实际上用 scan_realtime 但 ext_set 只含 .txt
            sc.scan_realtime(opts, big_ext, cb);
            auto t1 = Clock::now();
            double ms = elapsed_ms(t0, t1);
            size_t mem = get_process_memory_mb();

            // 获取文件大小
            std::error_code ec;
            auto fsize = std::filesystem::file_size(bigfile, ec);
            double fmb = ec ? 0 : static_cast<double>(fsize) / (1024 * 1024);

            std::cout << "    \"bigfile_streaming\": {\n";
            std::cout << "      \"file_size_mb\": " << std::fixed
                      << std::setprecision(2) << fmb << ",\n";
            std::cout << "      \"results\": " << all.size() << ",\n";
            std::cout << "      \"elapsed_ms\": " << std::fixed
                      << std::setprecision(2) << ms << ",\n";
            std::cout << "      \"peak_memory_mb\": " << mem << "\n";
            std::cout << "    }\n";

            // 清理大文件
            std::filesystem::remove(bigfile, ec);
        }
    }

    // 清理测试目录
    std::error_code ec;
    std::filesystem::remove_all(corpus, ec);
    // 清理可能残留的索引
    std::filesystem::remove_all(corpus / ".textsearch", ec);

    std::cout << "  }\n";
    std::cout << "}\n";

    return 0;
}
