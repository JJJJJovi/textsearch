// core/matcher.h — 匹配引擎接口
#pragma once

#include "core/types.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// 匹配模式标志（位或组合）
enum MatchFlags {
    Substring  = 0,   // 子串模式（默认）
    Regex      = 1,   // 正则模式
    IgnoreCase = 2,   // 忽略大小写
    WholeWord  = 4,   // 全词匹配（附录 F.3.2）
};

// 正则检查结果
struct RegexCheckResult {
    bool valid = true;              // 正则是否有效
    bool redos_warning = false;     // ReDoS 风险警告
    std::wstring message;           // 错误或警告消息
};

// 在文本中搜索匹配
// - text:      已解码的完整文本（wstring，UTF-16LE）
// - pattern:   搜索关键词 / 正则表达式
// - flags:     MatchFlags 位或组合
// - file_path: 填入 MatchResult.file_path
// - max_results: 结果上限（达到即停止收集）
// - timeout_sec: 单文件超时（秒，0=不限）。仅正则分支生效：逐行检查处
//   超过截止点即 break，返回已得结果（S4：兑现 README「单文件正则超时」）。
//   调用点透传 opts.search_timeout_sec（默认 120s）。
std::vector<MatchResult>
search_in_text(const std::wstring& text,
               const std::wstring& pattern,
               int flags,
               const std::filesystem::path& file_path = {},
               size_t max_results = SIZE_MAX,
               double timeout_sec = 0.0);

// 检查正则语法有效性 + ReDoS 风险（附录 F.2.3）
RegexCheckResult check_regex(const std::wstring& pattern, bool ignore_case);

// 启发式检测嵌套量词 ReDoS 风险（附录 F.2.3）
// 检测 (x+)+、(x*)*、(x+)*、(x*)+ 等模式
bool has_redos_risk(const std::wstring& pattern);

// 设置外部取消标志（用于正则模式逐行中断检查）
// 传入 nullptr 取消取消检查
void set_regex_cancel_flag(const std::atomic<bool>* flag);
