// core/matcher.cpp — 匹配引擎实现
#include "core/matcher.h"
#include "core/util.h"

#include <chrono>
#include <regex>
#include <cwctype>

// 全局取消标志指针（由外部 set_regex_cancel_flag 设置）。
// atomic 指针本体：setter（scanner 线程）与 matcher worker 线程的读并发，
// 裸指针是数据竞争；relaxed 足够（只需最终看到，不需同步其他内存）。
static std::atomic<const std::atomic<bool>*> g_cancel_flag{nullptr};

void set_regex_cancel_flag(const std::atomic<bool>* flag) {
    g_cancel_flag.store(flag, std::memory_order_relaxed);
}

// ---- 行切分 ----

struct LineInfo {
    int line_no;       // 1-indexed
    std::wstring text; // 行文本（已去除行尾 \r）
};

static std::vector<LineInfo> split_lines(const std::wstring& text) {
    std::vector<LineInfo> lines;
    int line_no = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == L'\n') {
            ++line_no;
            std::wstring line = text.substr(start, i - start);
            // 去除行尾 \r（R6：二进制模式读，\r\n 不被自动转换）
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }
            lines.push_back({line_no, std::move(line)});
            start = i + 1;
        }
    }
    return lines;
}

// ---- 全词边界检查 ----

static bool is_whole_word(const std::wstring& line, size_t start, size_t length) {
    // 检查命中位置前一个字符
    if (start > 0) {
        wchar_t prev = line[start - 1];
        if (is_word_char(prev)) return false;
    }
    // 检查命中位置后一个字符
    size_t after = start + length;
    if (after < line.size()) {
        wchar_t next = line[after];
        if (is_word_char(next)) return false;
    }
    return true;
}

// ---- ReDoS 风险检测（附录 F.2.3）----

bool has_redos_risk(const std::wstring& pattern) {
    // 启发式检测嵌套量词：(x+)+, (x*)*, (x+)*, (x*)+
    // 策略：用栈跟踪分组，当 ) 后跟 + 或 * 时检查组内是否含量词
    std::vector<size_t> group_stack;
    for (size_t i = 0; i < pattern.size(); ++i) {
        // 跳过转义字符
        if (pattern[i] == L'\\' && i + 1 < pattern.size()) {
            ++i;
            continue;
        }
        if (pattern[i] == L'(') {
            group_stack.push_back(i);
        } else if (pattern[i] == L')' && !group_stack.empty()) {
            size_t start = group_stack.back();
            group_stack.pop_back();

            // 检查 ) 后是否跟 + 或 * 量词
            if (i + 1 < pattern.size() &&
                (pattern[i + 1] == L'+' || pattern[i + 1] == L'*')) {
                // 检查组内是否含量词（+, *, {n,}）
                bool has_quant = false;
                for (size_t j = start + 1; j < i; ++j) {
                    if (pattern[j] == L'\\' && j + 1 < pattern.size()) {
                        ++j;
                        continue;
                    }
                    if (pattern[j] == L'+' || pattern[j] == L'*') {
                        has_quant = true;
                        break;
                    }
                    if (pattern[j] == L'{' ) {
                        // 简化处理：{ 视为量词
                        has_quant = true;
                        break;
                    }
                }
                if (has_quant) return true;
            }
        }
    }
    return false;
}

// ---- 正则检查 ----

RegexCheckResult check_regex(const std::wstring& pattern, bool ignore_case) {
    RegexCheckResult result;
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (ignore_case) flags |= std::regex_constants::icase;
        std::wregex re(pattern, flags); // 尝试编译
        (void)re; // 编译成功，丢弃
    } catch (const std::regex_error& e) {
        result.valid = false;
        std::string what = e.what();
        result.message = utf8_to_wstring(what);
        return result;
    }
    // 检查 ReDoS 风险
    if (has_redos_risk(pattern)) {
        result.redos_warning = true;
        result.message = L"正则可能存在性能风险（嵌套量词）";
    }
    return result;
}

// ---- 主搜索函数 ----

std::vector<MatchResult>
search_in_text(const std::wstring& text,
               const std::wstring& pattern,
               int flags,
               const std::filesystem::path& file_path,
               size_t max_results,
               double timeout_sec) {
    std::vector<MatchResult> results;

    // 空 pattern → 无结果（F.1.4）
    if (pattern.empty()) return results;

    bool use_regex = (flags & MatchFlags::Regex) != 0;
    bool ignore_case = (flags & MatchFlags::IgnoreCase) != 0;
    bool whole_word = (flags & MatchFlags::WholeWord) != 0;

    auto lines = split_lines(text);

    if (use_regex) {
        // ---- 正则模式 ----
        std::wregex re;
        try {
            auto re_flags = std::regex_constants::ECMAScript;
            if (ignore_case) re_flags |= std::regex_constants::icase;
            re = std::wregex(pattern, re_flags);
        } catch (...) {
            return results; // 编译失败，返回空
        }

        // S4：单文件正则超时（timeout_sec>0 时生效）。截止点在循环里逐行
        // 检查，超时 break 返回已得结果（部分结果优于卡死整次搜索）。
        bool use_deadline = timeout_sec > 0.0;
        std::chrono::steady_clock::time_point deadline;
        if (use_deadline) {
            auto us = static_cast<long long>(timeout_sec * 1e6);
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(us);
        }

        for (const auto& [ln, line] : lines) {
            // 附录 F.2.3：每行检查取消标志
            const std::atomic<bool>* cf = g_cancel_flag.load(std::memory_order_relaxed);
            if (cf && cf->load(std::memory_order_relaxed)) {
                break;
            }
            // S4：单文件正则超时
            if (use_deadline && std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            std::wsmatch m;
            std::wstring::const_iterator search_start = line.cbegin();
            const std::wstring::const_iterator line_end = line.cend();

            while (search_start != line_end &&
                   std::regex_search(search_start, line_end, m, re)) {
                size_t match_pos = static_cast<size_t>(m[0].first - line.cbegin());
                size_t match_len = static_cast<size_t>(m[0].second - m[0].first);
                int col = static_cast<int>(match_pos) + 1; // 1-indexed

                if (!whole_word || is_whole_word(line, match_pos, match_len)) {
                    results.push_back({ln, col, line, file_path});
                    if (results.size() >= max_results) return results;
                }

                // 推进到匹配末尾
                search_start = m[0].second;
                // 零长度匹配：推进一位避免死循环
                if (m[0].first == m[0].second) {
                    ++search_start;
                }
            }
        }
    } else {
        // ---- 子串模式（R20：绝不送正则引擎，用 wstring::find）----
        std::wstring pat = ignore_case ? to_lower_wstring(pattern) : pattern;

        for (const auto& [ln, line] : lines) {
            const std::wstring& search_line = ignore_case ? to_lower_wstring(line) : line;

            size_t pos = 0;
            while (true) {
                pos = search_line.find(pat, pos);
                if (pos == std::wstring::npos) break;

                int col = static_cast<int>(pos) + 1; // 1-indexed

                if (!whole_word || is_whole_word(search_line, pos, pat.size())) {
                    results.push_back({ln, col, line, file_path});
                    if (results.size() >= max_results) return results;
                }

                pos += pat.size();
                // 防止空 pattern 死循环（前面已拦截，此处保险）
                if (pat.empty()) break;
            }
        }
    }

    return results;
}
