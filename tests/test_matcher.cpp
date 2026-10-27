// tests/test_matcher.cpp — 匹配引擎测试
#include "simpletest.h"
#include "core/matcher.h"

#include <chrono>
#include <cstddef>
#include <filesystem>

// ---- 子串模式测试 ----

TEST_CASE(match_substring_basic) {
    std::wstring text = L"中文文本\n第二行";
    auto results = search_in_text(text, L"文本", MatchFlags::Substring);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].line_no, 1);
    REQUIRE_EQ(results[0].col, 3); // 第3个字符（1-indexed）
    REQUIRE_EQ(results[0].line_text, L"中文文本");
}

TEST_CASE(match_substring_no_match) {
    std::wstring text = L"中文文本";
    auto results = search_in_text(text, L"不存在", MatchFlags::Substring);
    REQUIRE(results.empty());
}

TEST_CASE(match_substring_ignore_case) {
    std::wstring text = L"Hello World hello";
    auto results = search_in_text(text, L"hello", MatchFlags::Substring | MatchFlags::IgnoreCase);
    REQUIRE_EQ(results.size(), static_cast<size_t>(2));
    REQUIRE_EQ(results[0].line_no, 1);
    REQUIRE_EQ(results[0].col, 1);
    REQUIRE_EQ(results[1].col, 13);
}

TEST_CASE(match_substring_case_sensitive) {
    std::wstring text = L"Hello World hello";
    auto results = search_in_text(text, L"hello", MatchFlags::Substring);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 13);
}

TEST_CASE(match_substring_multi_hit_same_line) {
    std::wstring text = L"ab ab ab";
    auto results = search_in_text(text, L"ab", MatchFlags::Substring);
    REQUIRE_EQ(results.size(), static_cast<size_t>(3));
    REQUIRE_EQ(results[0].col, 1);
    REQUIRE_EQ(results[1].col, 4);
    REQUIRE_EQ(results[2].col, 7);
}

TEST_CASE(match_substring_multi_line) {
    std::wstring text = L"foo\nbar\nfoo\nbaz";
    auto results = search_in_text(text, L"foo", MatchFlags::Substring);
    REQUIRE_EQ(results.size(), static_cast<size_t>(2));
    REQUIRE_EQ(results[0].line_no, 1);
    REQUIRE_EQ(results[1].line_no, 3);
}

TEST_CASE(match_substring_empty_pattern) {
    std::wstring text = L"some text";
    auto results = search_in_text(text, L"", MatchFlags::Substring);
    REQUIRE(results.empty());
}

TEST_CASE(match_substring_crlf_handling) {
    // 二进制模式下 \r\n 不被转换，行尾 \r 应被去除
    std::wstring text = L"line1\r\nline2\r\n";
    auto results = search_in_text(text, L"line1", MatchFlags::Substring);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].line_text, L"line1"); // 不含 \r
    REQUIRE_EQ(results[0].col, 1);
}

TEST_CASE(match_substring_with_file_path) {
    std::wstring text = L"test content";
    std::filesystem::path fp = L"C:\\fake\\path.c";
    auto results = search_in_text(text, L"test", MatchFlags::Substring, fp);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].file_path, fp);
}

TEST_CASE(match_substring_max_results) {
    std::wstring text = L"a a a a a";
    auto results = search_in_text(text, L"a", MatchFlags::Substring, {}, 3);
    REQUIRE_EQ(results.size(), static_cast<size_t>(3));
}

// ---- 正则模式测试 ----

TEST_CASE(match_regex_basic) {
    std::wstring text = L"abc 123 def";
    auto results = search_in_text(text, L"\\d+", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 5);
}

TEST_CASE(match_regex_ignore_case) {
    std::wstring text = L"Hello HELLO hello";
    auto results = search_in_text(text, L"hello", MatchFlags::Regex | MatchFlags::IgnoreCase);
    REQUIRE_EQ(results.size(), static_cast<size_t>(3));
}

TEST_CASE(match_regex_case_sensitive) {
    std::wstring text = L"Hello HELLO hello";
    auto results = search_in_text(text, L"hello", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 13);
}

TEST_CASE(match_regex_multi_line) {
    std::wstring text = L"123\n456\nabc";
    auto results = search_in_text(text, L"\\d+", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(2));
    REQUIRE_EQ(results[0].line_no, 1);
    REQUIRE_EQ(results[1].line_no, 2);
}

TEST_CASE(match_regex_email) {
    std::wstring text = L"contact: test@example.com end";
    auto results = search_in_text(text, L"[a-z]+@[a-z]+\\.[a-z]+", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 10);
}

TEST_CASE(match_regex_date) {
    std::wstring text = L"date 2026-08-26 here";
    auto results = search_in_text(text, L"\\d{4}-\\d{2}-\\d{2}", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 6);
}

// ---- 全词匹配测试 ----

TEST_CASE(match_whole_word_substring) {
    std::wstring text = L"foo foobar foo bar";
    auto results = search_in_text(text, L"foo", MatchFlags::Substring | MatchFlags::WholeWord);
    REQUIRE_EQ(results.size(), static_cast<size_t>(2));
    REQUIRE_EQ(results[0].col, 1);
    REQUIRE_EQ(results[1].col, 12);
}

TEST_CASE(match_whole_word_not_partial) {
    std::wstring text = L"var myVar = 1";
    auto results = search_in_text(text, L"var", MatchFlags::Substring | MatchFlags::WholeWord);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1));
    REQUIRE_EQ(results[0].col, 1); // 只匹配 "var"，不匹配 "myVar" 中的 var
}

TEST_CASE(match_whole_word_regex) {
    std::wstring text = L"cat category cat";
    auto results = search_in_text(text, L"cat", MatchFlags::Regex | MatchFlags::WholeWord);
    REQUIRE_EQ(results.size(), static_cast<size_t>(2));
    REQUIRE_EQ(results[0].col, 1);
    REQUIRE_EQ(results[1].col, 14);
}

// ---- ReDoS 防护测试 ----

TEST_CASE(match_redos_nested_plus) {
    REQUIRE(has_redos_risk(L"(a+)+"));
}

TEST_CASE(match_redos_nested_star) {
    REQUIRE(has_redos_risk(L"(a*)*"));
}

TEST_CASE(match_redos_nested_mixed) {
    REQUIRE(has_redos_risk(L"(a+)*"));
}

TEST_CASE(match_redos_no_risk_simple) {
    REQUIRE(!has_redos_risk(L"abc"));
}

TEST_CASE(match_redos_no_risk_group) {
    REQUIRE(!has_redos_risk(L"(abc)+")); // 组内无量词
}

TEST_CASE(match_redos_no_risk_alt) {
    REQUIRE(!has_redos_risk(L"(a|b)+")); // | 不是量词
}

TEST_CASE(match_check_regex_valid) {
    auto result = check_regex(L"\\d+", false);
    REQUIRE(result.valid);
    REQUIRE(!result.redos_warning);
}

TEST_CASE(match_check_regex_invalid) {
    auto result = check_regex(L"[invalid", false);
    REQUIRE(!result.valid);
}

TEST_CASE(match_check_regex_redos_warning) {
    auto result = check_regex(L"(a+)+", false);
    REQUIRE(result.valid);         // 语法有效
    REQUIRE(result.redos_warning);  // 但有 ReDoS 警告
}

// ---- 取消标志测试 ----

TEST_CASE(match_cancel_flag) {
    std::atomic<bool> cancelled(false);
    set_regex_cancel_flag(&cancelled);

    // 大文本，取消标志为 false → 正常搜索
    std::wstring text(1000, L'a');
    auto results = search_in_text(text, L"a", MatchFlags::Regex);
    REQUIRE_EQ(results.size(), static_cast<size_t>(1000));

    // 设置取消标志 → 中途停止
    cancelled.store(true);
    auto results2 = search_in_text(text, L"a", MatchFlags::Regex);
    REQUIRE(results2.empty()); // 第一行即被取消

    // 清除取消标志
    set_regex_cancel_flag(nullptr);
}

// ============================================================
// S4：单文件正则超时（timeout_sec 参数，0=不限）
// ============================================================

TEST_CASE(matcher_regex_timeout) {
    // 灾难性回溯模式 (a+)+b：每行都无匹配 → 每行回溯；海量行使总耗时
    // 远超 deadline。旧实现无超时参数会一直跑；新实现应在 deadline 附近
    // 返回已得结果（此处无匹配 → 空）。
    std::wstring text;
    text.reserve(400000);
    for (int i = 0; i < 20000; ++i) text += L"aaaaaaaaaaaaaaaa!\n";

    auto t0 = std::chrono::steady_clock::now();
    auto results = search_in_text(text, L"(a+)+b", MatchFlags::Regex,
                                  {}, static_cast<size_t>(-1), 0.05);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    REQUIRE(ms < 5000);       // 不挂死（粒度=每行一次检查，留足余量）
    REQUIRE(results.empty()); // (a+)+b 与语料无匹配
}

TEST_CASE(matcher_regex_within_time) {
    // deadline 宽裕 → 结果与不限时完全一致
    std::wstring text;
    for (int i = 0; i < 2000; ++i) text += L"foo1 foo2 bar\n";

    auto unlimited = search_in_text(text, L"foo\\d", MatchFlags::Regex);
    auto with_dl = search_in_text(text, L"foo\\d", MatchFlags::Regex,
                                  {}, static_cast<size_t>(-1), 30.0);
    REQUIRE_EQ(unlimited.size(), with_dl.size());
    REQUIRE(unlimited.size() >= static_cast<size_t>(4000));
}
