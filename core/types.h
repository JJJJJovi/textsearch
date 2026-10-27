// core/types.h — 公共数据结构定义
// 包含附录 F.3.1 / F.3.2 / F.3.3 的扩展规格
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// 编码类型枚举（附录 F.3.3 扩展：增加 Utf16Le / Utf16Be）
enum class Encoding {
    Unknown,   // 未知编码
    Utf8,      // UTF-8（含或不含 BOM）
    Gbk,       // GBK / GB2312（CP 936）
    Utf16Le,   // UTF-16 Little-Endian
    Utf16Be,   // UTF-16 Big-Endian
};

// 单条匹配结果（附录 F.3.1 扩展：增加 file_path）
struct MatchResult {
    int line_no = 0;                    // 行号（1-indexed）
    int col = 0;                        // 列号（1-indexed，基于 wchar_t 索引）
    std::wstring line_text;             // 命中行全文（已去除行尾 \r）
    std::filesystem::path file_path;   // 所属文件路径
};

// 搜索选项（附录 F.3.2 扩展：增加 whole_word / max_results / search_timeout_sec）
struct SearchOptions {
    std::filesystem::path root;                               // 搜索根目录
    std::wstring pattern;                                     // 搜索关键词 / 正则表达式
    bool recursive = true;                                    // 是否递归子目录
    bool use_regex = false;                                   // 正则模式
    bool ignore_case = false;                                 // 忽略大小写
    bool whole_word = false;                                  // 全词匹配
    size_t bigfile_threshold_mb = 50;                         // 大文件阈值（MB），超过走流式
    std::optional<std::pair<time_t, time_t>> mtime_filter;     // 修改时间过滤 [from, to]
    std::vector<std::wstring> blacklist;                     // 黑名单扩展名（精确匹配）
    std::vector<std::wstring> wildcards;                      // 通配排除模式（如 *.log*）
    bool force_realtime = false;                              // 强制即时扫描，跳过索引
    bool auto_update_index = true;                            // 走索引模式前先增量更新（保证结果不陈旧）
    size_t max_results = 100000;                              // 结果上限
    int search_timeout_sec = 120;   // 单文件正则超时（秒，0=不限；S4，2026-10 定值 120）
};

// 扫描统计
struct ScanStats {
    size_t files_done = 0;   // 已处理文件数
    size_t files_total = 0;  // 总文件数
    size_t skipped = 0;      // 跳过文件数（无权限 / 锁定 / 二进制）
};
