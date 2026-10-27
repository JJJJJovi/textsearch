// tests/test_indexer.cpp — 真实索引测试（T4b）
// 测试：建索引、子串搜索、正则搜索、增量更新、损坏恢复、串行 vs 索引一致性
#include "simpletest.h"
#include "core/indexer.h"
#include "core/scanner.h"
#include "core/encoding.h"
#include "core/filesystem.h"
#include "core/matcher.h"
#include "core/types.h"
#include "core/util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ---- 辅助：写 UTF-8 文件 ----
static void write_utf8_file(const std::filesystem::path& path,
                            const std::wstring& content) {
    std::string utf8 = wstring_to_utf8(content);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// ---- 辅助：写 GBK 文件 ----
static void write_gbk_file(const std::filesystem::path& path,
                           const std::wstring& content) {
    if (content.empty()) {
        std::ofstream ofs(path, std::ios::binary);
        return;
    }
    int len = WideCharToMultiByte(936, 0,
        content.c_str(), static_cast<int>(content.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        std::ofstream ofs(path, std::ios::binary);
        return;
    }
    std::string gbk(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(936, 0,
        content.c_str(), static_cast<int>(content.size()),
        gbk.data(), len, nullptr, nullptr);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(gbk.data(), static_cast<std::streamsize>(gbk.size()));
}

// ---- 辅助：结果比较键 ----
struct ResultKey {
    std::wstring path;
    int line = 0;
    int col = 0;
    bool operator<(const ResultKey& o) const {
        if (path != o.path) return path < o.path;
        if (line != o.line) return line < o.line;
        return col < o.col;
    }
    bool operator==(const ResultKey& o) const {
        return path == o.path && line == o.line && col == o.col;
    }
};

static std::set<ResultKey> to_keys(const std::vector<MatchResult>& results) {
    std::set<ResultKey> keys;
    for (const auto& r : results) {
        keys.insert({r.file_path.wstring(), r.line_no, r.col});
    }
    return keys;
}

// ---- 辅助：创建测试目录 ----
static std::filesystem::path make_test_dir(const std::wstring& name) {
    auto tmp = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    return tmp;
}

// ---- 辅助：同步即时扫描（收集所有结果）----
static std::vector<MatchResult> realtime_search(
    const std::filesystem::path& root,
    const std::wstring& pattern,
    const std::set<std::wstring>& ext_set,
    bool use_regex = false,
    bool ignore_case = false) {
    SearchOptions opts;
    opts.root = root;
    opts.pattern = pattern;
    opts.recursive = true;
    opts.use_regex = use_regex;
    opts.ignore_case = ignore_case;
    opts.force_realtime = true;  // 强制即时模式

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

// ---- 辅助：走索引路径搜索，收集 on_error 通道送出的告警/错误消息 ----
// （force_realtime=false → 命中索引分支；B3 的扩展名告警走的就是这条通道）
static std::vector<std::wstring> index_search_messages(
    const std::filesystem::path& root,
    const std::wstring& pattern,
    const std::set<std::wstring>& ext_set) {
    SearchOptions opts;
    opts.root = root;
    opts.pattern = pattern;
    opts.recursive = true;
    opts.force_realtime = false;        // 走索引
    opts.auto_update_index = true;

    std::vector<std::wstring> msgs;
    Scanner sc;
    ScanCallbacks cb;
    cb.on_results = [](std::vector<MatchResult>) {};
    cb.on_complete = []() {};
    cb.on_error = [&](const std::wstring& m) { msgs.push_back(m); };
    sc.scan_realtime(opts, ext_set, cb);
    return msgs;
}

// ============================================================
// 测试 1：建索引 → index_exists
// ============================================================

TEST_CASE(indexer_build_and_exists) {
    auto tmp = make_test_dir(L"textsearch_idx_build");
    // 创建 UTF-8 文件
    write_utf8_file(tmp / L"utf8.txt", L"hello world\n中文测试\nfoo bar\n");
    // 创建 GBK 文件
    write_gbk_file(tmp / L"gbk.txt", L"GBK 编码文件\n搜索中文内容\n12345\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    bool ok = idx.build(tmp, ext_set);
    REQUIRE(ok);
    REQUIRE(idx.is_available());
    REQUIRE(Indexer::index_exists(tmp));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 2：子串搜索 — 中文
// ============================================================

TEST_CASE(indexer_search_chinese_substring) {
    auto tmp = make_test_dir(L"textsearch_idx_search_cn");
    write_utf8_file(tmp / L"a.txt",
        L"第一行\n这是中文测试内容\n第三行\n搜索中文\n");
    write_gbk_file(tmp / L"b.txt",
        L"另一文件\n中文搜索内容在这里\n结束\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 搜索 "中文"（bi-gram: 中文）
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"中文";
    opts.recursive = true;
    opts.force_realtime = false;
    auto results = idx.search(L"中文", opts);

    // a.txt 第2行和第4行含"中文"，b.txt 第2行含"中文"
    REQUIRE(results.size() >= 3);
    // 验证命中行文本包含"中文"
    for (const auto& r : results) {
        REQUIRE(r.line_text.find(L"中文") != std::wstring::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 3：子串搜索 — 英文
// ============================================================

TEST_CASE(indexer_search_english_substring) {
    auto tmp = make_test_dir(L"textsearch_idx_search_en");
    write_utf8_file(tmp / L"code.c",
        L"int main() {\n    printf(\"hello world\");\n    return 0;\n}\n"
        L"// hello function\n");

    std::set<std::wstring> ext_set = {L".c"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 搜索 "hello"（ASCII 词项）
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"hello";
    opts.recursive = true;
    auto results = idx.search(L"hello", opts);

    // 第2行和第5行含 "hello"
    REQUIRE(results.size() >= 2);
    for (const auto& r : results) {
        REQUIRE(r.line_text.find(L"hello") != std::wstring::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 4：正则搜索 — trigram 加速路径
// ============================================================

TEST_CASE(indexer_search_regex_trigram) {
    auto tmp = make_test_dir(L"textsearch_idx_regex");
    write_utf8_file(tmp / L"data.txt",
        L"order_12345 shipped\norder_67890 pending\nno match here\n"
        L"func_abc 999\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 搜索 \d+ （无字面量段 ≥3 → 回退全文件）
    // 但搜 "order_\d+" 有字面量段 "order_" (len=6 ≥3) → trigram 加速
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"order_\\d+";
    opts.recursive = true;
    opts.use_regex = true;
    auto results = idx.search(L"order_\\d+", opts);

    // 第1行和第2行匹配
    REQUIRE(results.size() >= 2);
    for (const auto& r : results) {
        REQUIRE(r.line_text.find(L"order_") != std::wstring::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 5：正则搜索 — \d+ 无字面量段（回退全文件）
// ============================================================

TEST_CASE(indexer_search_regex_no_literal) {
    auto tmp = make_test_dir(L"textsearch_idx_regex_nolit");
    write_utf8_file(tmp / L"nums.txt",
        L"line with 123 number\nanother 456 here\nno numbers\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"\\d+";
    opts.recursive = true;
    opts.use_regex = true;
    auto results = idx.search(L"\\d+", opts);

    // 第1行和第2行匹配数字
    REQUIRE(results.size() >= 2);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 6：增量更新 — 修改文件后命中更新
// ============================================================

TEST_CASE(indexer_update_incremental) {
    auto tmp = make_test_dir(L"textsearch_idx_incremental");
    write_utf8_file(tmp / L"f.txt", L"old content\nsearch this line\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 初始搜索 "old"
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"old";
    opts.recursive = true;
    auto before = idx.search(L"old", opts);
    REQUIRE(!before.empty());

    // 修改文件内容（等待 mtime 变化）
    std::this_thread::sleep_for(std::chrono::seconds(2));
    write_utf8_file(tmp / L"f.txt", L"new content\nupdated line\n");
    // 新增文件
    write_utf8_file(tmp / L"g.txt", L"new file added\nsearch newword\n");

    // 增量更新
    REQUIRE(idx.update_incremental());

    // 搜索 "old" → 应无结果（旧内容已删除）
    auto after_old = idx.search(L"old", opts);
    REQUIRE(after_old.empty());

    // 搜索 "new" → 应有结果（新内容）
    SearchOptions opts2;
    opts2.root = tmp;
    opts2.pattern = L"new";
    opts2.recursive = true;
    auto after_new = idx.search(L"new", opts2);
    REQUIRE(!after_new.empty());

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 6b：增量更新快速预检 + 变更计数
// 重点验证：① 无变化时不误报（changed=0，走快速预检不触发全量 load/save）
//          ② 有变化时能检出（changed>0）—— 若预检误判为"无变化"，
//             索引会永久陈旧且无提示，是最危险的静默错误
// ============================================================

TEST_CASE(indexer_incremental_fastpath_and_counts) {
    auto tmp = make_test_dir(L"textsearch_idx_fastpath");
    write_utf8_file(tmp / L"a.txt", L"alpha content\n");
    write_utf8_file(tmp / L"b.txt", L"bravo content\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;

    // 1) 无变化 → 快速预检判定"无需写回"，变更数为 0（不误报）
    size_t changed = 999;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed == 0);
    // 幂等：再调一次仍为 0
    changed = 999;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed == 0);

    // 2) 删除文件 → 检出 1 个变更，已删内容搜不到、其余仍能搜到
    std::filesystem::remove(tmp / L"b.txt");
    changed = 0;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed == 1);
    REQUIRE(idx.search(L"bravo", opts).empty());
    REQUIRE(!idx.search(L"alpha", opts).empty());

    // 3) 新增文件 → 检出 1 个变更，新内容能搜到
    //    注意：内容不要与 a.txt 的 "content" 重叠，否则会干扰步骤 4 的断言
    write_utf8_file(tmp / L"c.txt", L"charlie delta\n");
    changed = 0;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed == 1);
    REQUIRE(!idx.search(L"charlie", opts).empty());

    // 4) 修改文件（等 mtime 变化）→ 检出 1 个变更，旧词消失、新词命中
    std::this_thread::sleep_for(std::chrono::seconds(2));
    write_utf8_file(tmp / L"a.txt", L"alpha replaced\n");
    changed = 0;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed == 1);
    REQUIRE(idx.search(L"content", opts).empty());
    REQUIRE(!idx.search(L"replaced", opts).empty());

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 7：损坏恢复 — 截断索引文件 → verify_and_recover 返回 false
// ============================================================

TEST_CASE(indexer_corruption_recovery) {
    auto tmp = make_test_dir(L"textsearch_idx_corrupt");
    write_utf8_file(tmp / L"f.txt", L"some content here\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));
    REQUIRE(Indexer::index_exists(tmp));

    // 截断索引文件（模拟损坏）
    auto idx_path = tmp / ".textsearch" / "index.lmdb";
    {
        std::ofstream ofs(idx_path, std::ios::binary | std::ios::trunc);
        ofs.write("TRUNCATED", 9);  // 破坏 magic + checksum
    }

    // verify_and_recover 应返回 false（检测到损坏）
    bool ok = Indexer::verify_and_recover(tmp);
    REQUIRE(!ok);

    // 损坏后 index_exists 也应返回 false
    REQUIRE(!Indexer::index_exists(tmp));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 7b：index_exists 缓存 — 删除索引文件后必须返回 false（不读陈旧缓存）
// ============================================================

TEST_CASE(indexer_exists_cache_invalidation) {
    auto tmp = make_test_dir(L"textsearch_idx_cache_inval");
    write_utf8_file(tmp / L"f.txt", L"content for cache test\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));
    REQUIRE(Indexer::index_exists(tmp));   // 建库后：true（同时填充 mtime/size 缓存）

    // 直接删除索引文件，模拟"索引被外部清理 / 重建中"
    std::error_code ec;
    std::filesystem::remove_all(tmp / ".textsearch", ec);
    // 缓存里旧条目 mtime 已查不到（文件不存在）→ 必须 false，不能返回陈旧 true
    REQUIRE(!Indexer::index_exists(tmp));

    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 8：串行 vs 索引结果一致
// ============================================================

TEST_CASE(indexer_vs_realtime_consistency) {
    auto tmp = make_test_dir(L"textsearch_idx_consistency");
    write_utf8_file(tmp / L"a.txt",
        L"function foo() {\n    return 42;\n}\n// hello world\n");
    write_utf8_file(tmp / L"b.txt",
        L"another file\nfoo bar baz\nhello again\n");
    write_gbk_file(tmp / L"c.txt",
        L"中文内容\n搜索测试foo\n结束\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 搜索 "foo"（ASCII 词项）
    SearchOptions idx_opts;
    idx_opts.root = tmp;
    idx_opts.pattern = L"foo";
    idx_opts.recursive = true;

    auto idx_results = idx.search(L"foo", idx_opts);

    // 即时扫描
    auto rt_results = realtime_search(tmp, L"foo", ext_set);

    // 结果集应一致（比较 file+line+col）
    auto idx_keys = to_keys(idx_results);
    auto rt_keys = to_keys(rt_results);

    REQUIRE(idx_keys == rt_keys);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 9：串行 vs 索引结果一致（中文搜索）
// ============================================================

TEST_CASE(indexer_vs_realtime_chinese_consistency) {
    auto tmp = make_test_dir(L"textsearch_idx_cn_consistency");
    write_utf8_file(tmp / L"a.txt",
        L"第一行\n中文搜索测试\n第三行中文\n");
    write_gbk_file(tmp / L"b.txt",
        L"文件内容\n中文搜索在这里\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions idx_opts;
    idx_opts.root = tmp;
    idx_opts.pattern = L"中文搜索";
    idx_opts.recursive = true;

    auto idx_results = idx.search(L"中文搜索", idx_opts);
    auto rt_results = realtime_search(tmp, L"中文搜索", ext_set);

    auto idx_keys = to_keys(idx_results);
    auto rt_keys = to_keys(rt_results);

    REQUIRE(idx_keys == rt_keys);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 10：open 已有索引后搜索
// ============================================================

TEST_CASE(indexer_open_existing) {
    auto tmp = make_test_dir(L"textsearch_idx_open");
    write_utf8_file(tmp / L"f.txt", L"open test content\nsearch keyword\n");

    std::set<std::wstring> ext_set = {L".txt"};
    {
        Indexer build_idx;
        REQUIRE(build_idx.build(tmp, ext_set));
    }
    // build_idx 析构后索引仍在磁盘

    // 用新 Indexer 打开
    Indexer idx;
    REQUIRE(idx.open(tmp));
    REQUIRE(idx.is_available());

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"open";
    opts.recursive = true;
    auto results = idx.search(L"open", opts);
    REQUIRE(!results.empty());

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 11：空 pattern 返回空结果
// ============================================================

TEST_CASE(indexer_empty_pattern) {
    auto tmp = make_test_dir(L"textsearch_idx_empty");
    write_utf8_file(tmp / L"f.txt", L"some content\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"";
    opts.recursive = true;
    auto results = idx.search(L"", opts);
    REQUIRE(results.empty());

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 12：ignore_case 子串搜索
// ============================================================

TEST_CASE(indexer_ignore_case_substring) {
    auto tmp = make_test_dir(L"textsearch_idx_icase");
    write_utf8_file(tmp / L"f.txt",
        L"Hello World\nHELLO there\nlowercase hello\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"hello";
    opts.recursive = true;
    opts.ignore_case = true;
    auto results = idx.search(L"hello", opts);

    // 三行都含 hello（大小写不同）
    REQUIRE(results.size() >= 3);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 13：增量更新 — 删除文件后不再命中
// ============================================================

TEST_CASE(indexer_incremental_delete) {
    auto tmp = make_test_dir(L"textsearch_idx_inc_del");
    write_utf8_file(tmp / L"keep.txt", L"keep this\nunique_word here\n");
    write_utf8_file(tmp / L"del.txt", L"delete this\nunique_word too\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 搜索 unique_word → 应在两个文件中命中
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"unique_word";
    opts.recursive = true;
    auto before = idx.search(L"unique_word", opts);
    REQUIRE(before.size() >= 2);

    // 删除 del.txt
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::error_code ec;
    std::filesystem::remove(tmp / L"del.txt", ec);

    // 增量更新
    REQUIRE(idx.update_incremental());

    // 再搜 → 应只在 keep.txt 中命中
    auto after = idx.search(L"unique_word", opts);
    REQUIRE(after.size() >= 1);
    REQUIRE(after.size() < before.size());

    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 回归测试：索引模式下扩展名过滤必须生效（Q：取消勾选的扩展名不应出现）
// 复现：建索引含 .txt 与 .c，搜索时仅传 {.c}，结果不得包含任何 .txt
// ============================================================

TEST_CASE(indexer_ext_filter_excludes_unchecked) {
    auto tmp = make_test_dir(L"ts_idx_extfilter");
    std::error_code ec;

    // 两个不同扩展名文件，内容都含同一关键词 "TARGETWORD"
    write_utf8_file(tmp / L"a.txt", L"line one TARGETWORD here");
    write_utf8_file(tmp / L"b.c",   L"line one TARGETWORD here");

    // 建索引（ext_set 含两类，确保都被索引）
    std::set<std::wstring> build_ext = {L".txt", L".c"};
    Indexer idx;
    bool built = idx.build(tmp, build_ext);
    REQUIRE(built);

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"TARGETWORD";
    opts.recursive = true;

    // 仅搜索 .c：索引结果不得含 .txt
    std::set<std::wstring> only_c = {L".c"};
    auto c_results = idx.search(L"TARGETWORD", opts, only_c);
    for (const auto& r : c_results) {
        std::wstring ext = r.file_path.has_extension()
            ? to_lower_wstring(r.file_path.extension().wstring())
            : std::wstring();
        REQUIRE(ext == L".c");   // 任何 .txt 命中都应失败
    }
    REQUIRE(!c_results.empty()); // 至少 b.c 应命中

    // 仅搜索 .txt：不得含 .c（对称性验证）
    std::set<std::wstring> only_txt = {L".txt"};
    auto txt_results = idx.search(L"TARGETWORD", opts, only_txt);
    for (const auto& r : txt_results) {
        std::wstring ext = r.file_path.has_extension()
            ? to_lower_wstring(r.file_path.extension().wstring())
            : std::wstring();
        REQUIRE(ext == L".txt");
    }
    REQUIRE(!txt_results.empty());

    // 空集 = 不过滤：两类都应出现
    auto all_results = idx.search(L"TARGETWORD", opts);
    REQUIRE(all_results.size() >= 2);

    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试：下划线子词召回（2026-08-31 会话检索实验发现的 P0）
// ============================================================

TEST_CASE(indexer_underscore_subword_recall) {
    // is_word_char 把 '_' 视为词内字符，所以 mmap_index 整体是一个 term。
    // 修复前：搜 "mmap" 查 postings["mmap"] 找不到 mmap_index.cpp 所在行，
    // 导致整行漏报（实测真实语料中「mmap」漏 8/23、「mtime」漏 5/122）。
    // 修复方式：建索引时额外补记下划线子词，保留整词 term。
    auto tmp = make_test_dir(L"textsearch_idx_underscore");
    write_utf8_file(tmp / L"src.txt",
        L"see mmap_index.cpp and file_mtime_unix for detail\n"
        L"plain mmap here\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;

    // 连写标识符中的 "mmap" 必须能命中（修复前为 0 条）
    opts.pattern = L"mmap";
    auto r1 = idx.search(L"mmap", opts);
    REQUIRE_EQ(r1.size(), (size_t)2);

    // file_mtime_unix 中的 "mtime" 必须能命中（修复前为 0 条）
    opts.pattern = L"mtime";
    auto r2 = idx.search(L"mtime", opts);
    REQUIRE_EQ(r2.size(), (size_t)1);

    // 整词 term 必须仍然保留，语义不变
    opts.pattern = L"mmap_index";
    auto r3 = idx.search(L"mmap_index", opts);
    REQUIRE_EQ(r3.size(), (size_t)1);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

TEST_CASE(indexer_read_once_per_file_no_amplification) {
    // P1 修复验证：read_line 原本每次命中重读整个文件，放大可达数千倍
    // （实测 103MB 单文件 + 高频词 2M 命中直接卡死）。
    // 修复后：每个文件只读一次，命中行从缓存文本取。
    // 本测试用「高频词出现在大量行」做回归护栏：
    //   - 正确性：结果数 == 所有含该词的行数
    //   - 性能护栏：修复前每次命中重读整文件，此处会拖到数秒级（断言 < 5s 捕获回归）
    auto tmp = make_test_dir(L"textsearch_idx_readonce");

    const int kFiles = 3;
    const int kLinesPerFile = 25000;             // 每文件 25000 行，每行含 marker_xyz
    const size_t kExpected = (size_t)kFiles * kLinesPerFile;

    for (int f = 0; f < kFiles; ++f) {
        std::wstring content;
        for (int i = 0; i < kLinesPerFile; ++i) {
            // 约 60 字节一行，使单文件有可观体积（~1.5MB）
            content += L"id=" + std::to_wstring(f * 100000 + i) +
                       L" marker_xyz padding data here for amplification test line\n";
        }
        write_utf8_file(tmp / (L"f" + std::to_wstring(f) + L".txt"), content);
    }

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;

    auto t0 = std::chrono::steady_clock::now();
    auto results = idx.search(L"marker_xyz", opts);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    REQUIRE_EQ(results.size(), kExpected);
    // 放大回归护栏：修复前 9000 命中 × 每文件 ~180KB 重读 ≈ 1.6GB I/O，远超此阈值
    REQUIRE(ms < 5000.0);

    std::error_code ec2;
    std::filesystem::remove_all(tmp, ec2);
}

// ============================================================
// 测试 17：子串召回 —— 查询词是长 token 的内部子串（索引内容 v3）
// 重点验证：索引期补记的 ASCII 3-gram 子词 + 查询期 3-gram 展开 + 邻接验证。
//
// v2（整词 + 下划线子词）时代全部返回 0 的用例：
//   rdp    ⊂ wizardp          —— 纯子串，无下划线、无大小写边界
//   wizard ⊂ wizardp          —— 同上
//   Index / Reader ⊂ MmapIndexReader —— 驼峰段，下划线子词补记覆盖不到
// 这些是"索引 0 条 / 实时 N 条"的经典漏报，曾实测于真实语料
// （RDP：索引 0 / 实时 12，12 处全在 MUD 函数 wizardp 里）。
// 若此测试失败，说明 3-gram 补记或查询期展开被回退了。
// ============================================================

TEST_CASE(indexer_substring_recall_inside_token) {
    auto tmp = make_test_dir(L"textsearch_idx_substr_recall");
    // 注意大小写：搜索默认区分大小写，MmapIndexReader 里的 "Index"/"Reader"
    // 首字母大写，查询词必须同大小写才能命中（"mmap" 只匹配小写那几行）。
    write_utf8_file(tmp / L"code.c",
        L"int wizardp(void);\n"
        L"void MmapIndexReader(void);\n"
        L"int mmap_index = 0;\n"
        L"mmap();\n"
        L"return mmap_index + wizardp();\n"
        L"郭靖练就一身武功\n");

    std::set<std::wstring> ext_set = {L".c"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;

    // —— v3 修复前全部为 0 的用例（子串 / 驼峰段）——
    opts.pattern = L"rdp";                      // ⊂ wizardp（第 1、5 行）
    REQUIRE_EQ(idx.search(L"rdp", opts).size(), size_t(2));

    opts.pattern = L"wizard";                   // ⊂ wizardp
    REQUIRE_EQ(idx.search(L"wizard", opts).size(), size_t(2));

    opts.pattern = L"Index";                    // 驼峰段 ⊂ MmapIndexReader
    REQUIRE_EQ(idx.search(L"Index", opts).size(), size_t(1));

    opts.pattern = L"Reader";                   // 驼峰段 ⊂ MmapIndexReader
    REQUIRE_EQ(idx.search(L"Reader", opts).size(), size_t(1));

    opts.pattern = L"IndexReader";              // 跨两个驼峰段的连续子串
    REQUIRE_EQ(idx.search(L"IndexReader", opts).size(), size_t(1));

    // —— v2 时代就已正确的用例（整词 + 下划线子词），作为不回退护栏 ——
    opts.pattern = L"mmap";                     // 整词 + mmap_index 的下划线子词
    REQUIRE_EQ(idx.search(L"mmap", opts).size(), size_t(3));

    opts.pattern = L"mmap_index";               // 整词
    REQUIRE_EQ(idx.search(L"mmap_index", opts).size(), size_t(2));

    // —— CJK 走 bi-gram，不受 3-gram 改动影响，护栏防回归 ——
    opts.pattern = L"武功";
    REQUIRE_EQ(idx.search(L"武功", opts).size(), size_t(1));
    opts.pattern = L"郭靖";
    REQUIRE_EQ(idx.search(L"郭靖", opts).size(), size_t(1));

    // —— 不应有误报 ——
    opts.pattern = L"ReaderEx";
    REQUIRE(idx.search(L"ReaderEx", opts).empty());
    opts.pattern = L"mtime";
    REQUIRE(idx.search(L"mtime", opts).empty());

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 18：增量更新路径同样补记 3-gram 子词
// 重点验证：index_single_file()（update_incremental 走的路径）也写入
// 3-gram posting。v3 把该函数从"逐 token 读-改-写"改成了"按 term 批量
// 合并"，若批量化有误（或只在 build 路径补记了 3-gram），新增文件里的
// 长 token 就无法被子串查询命中 —— 且这种漏报只在增量后出现，全量重建
// 会掩盖它，属于最隐蔽的一类错误。
// ============================================================

TEST_CASE(indexer_incremental_substring_recall) {
    auto tmp = make_test_dir(L"textsearch_idx_incr_substr");
    write_utf8_file(tmp / L"f.c", L"int alpha(void);\n");

    std::set<std::wstring> ext_set = {L".c"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;

    // 增量前：wizardp 尚不存在
    REQUIRE(idx.search(L"rdp", opts).empty());

    // 等待 mtime 变化后新增文件（走 index_single_file 增量路径）
    std::this_thread::sleep_for(std::chrono::seconds(2));
    write_utf8_file(tmp / L"g.c",
        L"int wizardp(void);\n"
        L"void MmapIndexReader(void);\n");
    REQUIRE(idx.update_incremental());

    // 增量后：新文件里的长 token 必须能被内部子串召回
    opts.pattern = L"rdp";                      // ⊂ wizardp
    REQUIRE_EQ(idx.search(L"rdp", opts).size(), size_t(1));

    opts.pattern = L"Index";                    // 驼峰段 ⊂ MmapIndexReader
    REQUIRE_EQ(idx.search(L"Index", opts).size(), size_t(1));

    opts.pattern = L"wizardp";                  // 整词仍可用
    REQUIRE_EQ(idx.search(L"wizardp", opts).size(), size_t(1));

    // 旧文件条目未被破坏
    opts.pattern = L"alpha";
    REQUIRE_EQ(idx.search(L"alpha", opts).size(), size_t(1));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 19：大文件走 MMF 流式索引（🔴 索引器 OOM 修复回归）
// 用极小阈值(1MB)强迫一个 ~5MB 文件走 index_single_file_large（64MB 滑动
// 映射、逐行 tokenize、postings 攒满即 flush），验证：① 召回正确（没漏），
// ② 不走整文件 read_text（内存有界，不 OOM）。再与「阈值调到极大→走内存路径」
// 同内容建索引的召回数交叉校验，证明两条路径口径一致、无隐藏分歧。
// ============================================================

TEST_CASE(indexer_large_file_streaming) {
    const std::wstring MARKER = L"ZzzBigFileMarker";
    const size_t kMarkerLines = 1000;
    const size_t kTotalLines = 100000;   // 约 5MB ASCII，足以超过 1MB 阈值

    // 生成 ~5MB 内容：前 kMarkerLines 行含 MARKER，其余为 filler
    std::wstring content;
    content.reserve(kTotalLines * 52);
    for (size_t i = 0; i < kTotalLines; ++i) {
        if (i < kMarkerLines) {
            content += MARKER;
            content += L" some filler payload data line number ";
            content += std::to_wstring(i);
        } else {
            content += L"plain filler line payload data number ";
            content += std::to_wstring(i);
        }
        content += L"\n";
    }

    // —— 流式路径（阈值 1MB，~5MB 文件必走 MMF）——
    auto dir_large = make_test_dir(L"textsearch_idx_large_stream");
    write_utf8_file(dir_large / L"big.txt", content);

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx_large;
    idx_large.set_bigfile_threshold_mb(1);
    REQUIRE(idx_large.build(dir_large, ext_set));

    SearchOptions opts_large;
    opts_large.root = dir_large;
    opts_large.recursive = true;
    size_t n_large = idx_large.search(MARKER, opts_large).size();

    // —— 内存路径（阈值 100GB，~5MB 文件必走原 read_text 路径）——
    auto dir_small = make_test_dir(L"textsearch_idx_large_smallpath");
    write_utf8_file(dir_small / L"big.txt", content);

    Indexer idx_small;
    idx_small.set_bigfile_threshold_mb(100000);
    REQUIRE(idx_small.build(dir_small, ext_set));

    SearchOptions opts_small;
    opts_small.root = dir_small;
    opts_small.recursive = true;
    size_t n_small = idx_small.search(MARKER, opts_small).size();

    REQUIRE_EQ(n_large, kMarkerLines);   // 流式路径召回正确（且未 OOM 崩溃）
    REQUIRE_EQ(n_small, kMarkerLines);   // 内存路径召回正确
    REQUIRE_EQ(n_large, n_small);        // 两条索引路径口径一致

    std::error_code ec;
    std::filesystem::remove_all(dir_large, ec);
    std::filesystem::remove_all(dir_small, ec);
}

// ============================================================
// 测试 20：B3 —— 勾选了索引中不存在的扩展名必须明确告警
// 索引只含建库时那几种类型。用户勾了索引里没有的类型时，搜索会静默少结果，
// 用户会误判成「没有匹配」。此处验证三态：
//   ① 勾选集合 == 索引集合        → 不告警（不能误报刷屏）
//   ② 勾选集合 ⊃ 索引集合（含缺失）→ 告警，且点名缺失类型
//   ③ 勾选集合为空（= 不过滤）    → 不告警（无从判定，保守沉默）
// ============================================================

TEST_CASE(scanner_warn_unindexed_extensions) {
    auto tmp = make_test_dir(L"textsearch_b3_extwarn");
    write_utf8_file(tmp / L"a.txt", L"NeedleToken txt line\n");
    write_utf8_file(tmp / L"b.md", L"NeedleToken md line\n");

    // —— 只用 .txt 建库，.md 故意不进索引 ——
    std::set<std::wstring> build_ext = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, build_ext));

    // 前置：索引确实记录了建库时的扩展名集合
    std::set<std::wstring> recorded = idx.indexed_ext_set();
    REQUIRE_EQ(recorded.size(), size_t(1));
    REQUIRE_EQ(*recorded.begin(), std::wstring(L".txt"));

    // ① 勾选 == 索引 → 安静
    auto msgs_same = index_search_messages(tmp, L"NeedleToken", build_ext);
    REQUIRE_EQ(msgs_same.size(), size_t(0));

    // ② 多勾了索引里没有的 .md → 必须告警且点名
    std::set<std::wstring> wide_ext = {L".txt", L".md"};
    auto msgs_wide = index_search_messages(tmp, L"NeedleToken", wide_ext);
    bool warned = false;
    for (const auto& m : msgs_wide) {
        if (m.find(L"警告") != std::wstring::npos &&
            m.find(L".md") != std::wstring::npos) {
            warned = true;
        }
    }
    REQUIRE(warned);

    // ③ 空勾选集合（语义 = 不过滤）→ 无从判定，保守不告警
    auto msgs_empty =
        index_search_messages(tmp, L"NeedleToken", std::set<std::wstring>{});
    REQUIRE_EQ(msgs_empty.size(), size_t(0));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// O8：建索引后 .textsearch 目录带隐藏属性
// —— IndexStore::save 应对 <root>/.textsearch 调 SetFileAttributesW
//    加上 FILE_ATTRIBUTE_HIDDEN；核心扫描器不跳过隐藏目录，故不影响重建。
// ============================================================
#ifdef _WIN32
TEST_CASE(indexer_index_dir_hidden_attr) {
    auto tmp = make_test_dir(L"textsearch_o8_hidden");
    write_utf8_file(tmp / L"a.txt", L"hello world\n中文内容\nfoo bar\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));
    REQUIRE(Indexer::index_exists(tmp));

    // .textsearch 目录应存在、仍是目录、且带隐藏属性
    std::filesystem::path dir = tmp / L".textsearch";
    DWORD attr = GetFileAttributesW(dir.c_str());
    REQUIRE(attr != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
    REQUIRE((attr & FILE_ATTRIBUTE_HIDDEN) != 0);

    // 搜索功能不受隐藏属性影响（重建后仍能查到）
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"foo";
    opts.recursive = true;
    opts.force_realtime = false;
    auto results = idx.search(L"foo", opts);
    REQUIRE(results.size() >= 1);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}
#endif

// ============================================================
// S1.1：大小写敏感正则的 trigram 查询统一小写
// 索引期 trigram 只存小写（extract_trigrams(line, true)）；查询期若按
// ignore_case 决定是否 to_lower，大小写敏感 pattern 会查不到桶 → 漏报。
// trigram 只做候选筛选，大小写精筛由 search_in_text 的 flags 负责。
// ============================================================

TEST_CASE(indexer_regex_case_sensitive_hit) {
    auto tmp = make_test_dir(L"textsearch_s11_cs_hit");
    write_utf8_file(tmp / L"a.txt", L"FooBar baz\nnothing here\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 大小写敏感 pattern "Foo" → 必须命中第 1 行
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"Foo";
    opts.recursive = true;
    opts.use_regex = true;
    opts.ignore_case = false;
    opts.force_realtime = false;
    auto results = idx.search(L"Foo", opts);
    REQUIRE(!results.empty());
    for (const auto& r : results) {
        REQUIRE(r.line_text.find(L"Foo") != std::wstring::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

TEST_CASE(indexer_regex_case_sensitive_no_false_positive) {
    auto tmp = make_test_dir(L"textsearch_s11_cs_fp");
    // 只有 FooBar（大写 F），不含小写 foo
    write_utf8_file(tmp / L"a.txt", L"FooBar baz\nQuux Foo\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // pattern "foo" 大小写敏感 → 0 命中
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"foo";
    opts.recursive = true;
    opts.use_regex = true;
    opts.ignore_case = false;
    opts.force_realtime = false;
    auto results = idx.search(L"foo", opts);
    REQUIRE_EQ(results.size(), size_t(0));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// S1.2：alternation 候选集语义 —— "abcd|efgh" 候选应为
// files(abcd) ∪ files(efgh)，而非交集（旧实现返回 0，bug 最小复现）。
// ============================================================

TEST_CASE(indexer_regex_alternation_union) {
    auto tmp = make_test_dir(L"textsearch_s12_union");
    write_utf8_file(tmp / L"a.txt", L"prefix abcd suffix\n");
    write_utf8_file(tmp / L"b.txt", L"prefix efgh suffix\n");
    write_utf8_file(tmp / L"c.txt", L"nothing here\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"abcd|efgh";
    opts.recursive = true;
    opts.use_regex = true;
    opts.force_realtime = false;
    auto results = idx.search(L"abcd|efgh", opts);

    // 两文件各命中 1 次（当前实现返回 0 = bug）
    REQUIRE_EQ(results.size(), size_t(2));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// S1.2：可选组 "(xyz)?abc" —— 含裸 abc 的行必须命中
// （组可选 ⇒ 组内 run 不能参与候选交集；否则漏报）
// ============================================================

TEST_CASE(indexer_regex_optional_group) {
    auto tmp = make_test_dir(L"textsearch_s12_optgroup");
    write_utf8_file(tmp / L"a.txt", L"plain abc line\n");
    write_utf8_file(tmp / L"b.txt", L"xyzabc together\n");
    write_utf8_file(tmp / L"c.txt", L"xyz not here\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"(xyz)?abc";
    opts.recursive = true;
    opts.use_regex = true;
    opts.force_realtime = false;
    auto results = idx.search(L"(xyz)?abc", opts);

    // a.txt（裸 abc）与 b.txt（xyzabc）都命中
    bool a_hit = false, b_hit = false;
    for (const auto& r : results) {
        if (r.line_text.find(L"plain abc") != std::wstring::npos) a_hit = true;
        if (r.line_text.find(L"xyzabc") != std::wstring::npos) b_hit = true;
    }
    REQUIRE(a_hit);
    REQUIRE(b_hit);

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// S2.1：build 大文件流式（阈值 1MB + ~2.4MB 文件走 MMF 路径）
// 建库成功、内容可命中、行号正确，且与实时模式结果集一致。
// ============================================================

TEST_CASE(indexer_build_bigfile_streamed) {
    auto tmp = make_test_dir(L"textsearch_s21_bigfile");
    write_utf8_file(tmp / L"small.txt",
        L"shared line\nBIGFILE_NEEDLE_AA55 in small\n");
    {
        std::ofstream ofs(tmp / L"big.txt", std::ios::binary);
        const std::string filler =
            "log line filler abcdefghij klmnopqrst uvwxyz 0123456789 "
            "padding padding padding more padding here\n";
        for (int i = 0; i < 30000; ++i) {
            if (i == 12345) { ofs << "CRITICAL BIGFILE_NEEDLE_AA55 embedded line here\n"; }
            else if (i == 1 || i == 15000 || i == 29998) {
                ofs << "rare RARE777 token on this line " << i << "\n";
            } else { ofs << filler; }
        }
    }
    auto bsize = std::filesystem::file_size(tmp / L"big.txt");
    REQUIRE(bsize > 1024u * 1024u);   // 确认超过 1MB 阈值

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    idx.set_bigfile_threshold_mb(1);
    REQUIRE(idx.build(tmp, ext_set));

    // 大文件内容命中且行号正确（第 12346 行）
    SearchOptions opts;
    opts.root = tmp;
    opts.pattern = L"BIGFILE_NEEDLE_AA55";
    opts.recursive = true;
    opts.force_realtime = false;
    auto results = idx.search(L"BIGFILE_NEEDLE_AA55", opts);

    bool big_hit = false, small_hit = false;
    for (const auto& r : results) {
        REQUIRE(r.line_text.find(L"BIGFILE_NEEDLE_AA55") != std::wstring::npos);
        if (r.file_path.filename() == L"big.txt") {
            big_hit = true;
            REQUIRE_EQ(r.line_no, 12346);
        }
        if (r.file_path.filename() == L"small.txt") small_hit = true;
    }
    REQUIRE(big_hit);
    REQUIRE(small_hit);

    // 索引 vs 实时一致性（大文件变体，含跨多行的稀有 token）
    auto rt_results = realtime_search(tmp, L"RARE777", ext_set);
    SearchOptions opts2 = opts;
    opts2.pattern = L"RARE777";
    auto idx_rare = idx.search(L"RARE777", opts2);
    REQUIRE_EQ(idx_rare.size(), size_t(3));
    REQUIRE(to_keys(idx_rare) == to_keys(rt_results));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// S2.2：增量路径跳过二进制 + 变更文件变二进制时移除条目
// 旧实现 index_single_file 无 is_textual 探测：含 NUL 的 .txt 会被
// 当作文本入库（BIGBIN999 可被搜到 = 漏报方向的反向症状）。
// ============================================================

TEST_CASE(indexer_incremental_skips_binary) {
    auto tmp = make_test_dir(L"textsearch_s22_bin");
    write_utf8_file(tmp / L"a.txt", L"OLDTEXT111 alpha line\n");
    write_utf8_file(tmp / L"b.txt", L"BETA222 beta line\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    // 新增：含 NUL 的 .txt（二进制）+ 正常新文本
    {
        std::ofstream ofs(tmp / L"bin.txt", std::ios::binary);
        std::string s = "HEAD0001";
        s.push_back('\0');
        s += "BIGBIN999";
        s.push_back('\0');
        s += "\r\n\xff\xfe\x80 garbage tail\n";
        ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    write_utf8_file(tmp / L"c.txt", L"NEWTXT333 gamma line\n");

    size_t cnt = 0;
    REQUIRE(idx.update_incremental(&cnt));
    // 只有 c.txt 计入变更（二进制文件被跳过，不算变更）
    REQUIRE_EQ(cnt, size_t(1));

    // 二进制内容不得入索引
    SearchOptions opts;
    opts.root = tmp;
    opts.recursive = true;
    opts.force_realtime = false;
    opts.pattern = L"BIGBIN999";
    REQUIRE_EQ(idx.search(L"BIGBIN999", opts).size(), size_t(0));
    opts.pattern = L"HEAD0001";
    REQUIRE_EQ(idx.search(L"HEAD0001", opts).size(), size_t(0));
    // 新文本文件正常入索引
    auto found = idx.search(L"NEWTXT333", opts);
    REQUIRE_EQ(found.size(), size_t(1));

    // 变更文件变二进制 → 原索引条目移除
    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        std::ofstream ofs(tmp / L"a.txt", std::ios::binary);
        std::string s = "OLDTEXT111 now binary";
        s.push_back('\0');
        s += "\xff\xfe more junk padding bytes to change size\n";
        ofs.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    REQUIRE(idx.update_incremental(&cnt));
    REQUIRE_EQ(idx.search(L"OLDTEXT111", opts).size(), size_t(0));
    // 其余文件不受影响
    REQUIRE_EQ(idx.search(L"BETA222", opts).size(), size_t(1));

    // 幂等：目录内仅剩"预期不索引"的二进制文件时，增量预检应稳定为无变更
    REQUIRE(idx.update_incremental(&cnt));
    REQUIRE_EQ(cnt, size_t(0));

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// S1.2：README 第五节矩阵正则逐个「索引 vs 实时」结果集相等
// ============================================================

TEST_CASE(indexer_vs_realtime_regex_matrix) {
    auto tmp = make_test_dir(L"textsearch_s12_matrix");
    // 语料覆盖：字面段、无字面段、alternation、可选组、字符类、大小写
    write_utf8_file(tmp / L"a.txt",
        L"fooXXbar line\nfoobar too\nIndexer deserialize_postings\n"
        L"abc123def 4567\nXfooY\nabcd efgh\nplain abc here\n");
    write_utf8_file(tmp / L"b.txt",
        L"xyzabc joined\nFOOBAR upper\nindex lowercase\n"
        L"efgh tail\nno digits here\n张三丰 中文搜索 index索引\n");

    std::set<std::wstring> ext_set = {L".txt"};
    Indexer idx;
    REQUIRE(idx.build(tmp, ext_set));

    struct Case {
        std::wstring pattern;
        bool ignore_case = false;
    };
    const Case cases[] = {
        {L"foo.*bar", false},        // 矩阵：正则（有字面段）
        {L"\\d+", false},            // 矩阵：正则（无字面段）
        {L"Index", false},           // 矩阵：大小写敏感
        {L"index", false},
        {L"Index", true},            // 矩阵：忽略大小写
        {L"abcd|efgh", false},       // alternation 并集
        {L"(xyz)?abc", false},       // 可选组
        {L"[ab]cd[ef]g", false},     // 字符类（无 ≥3 字面段 → 全扫）
        {L"foo|bar", false},         // 短 run alternation
        {L"中文搜索", false},         // 正则模式下的中文
    };

    for (const auto& c : cases) {
        SearchOptions opts;
        opts.root = tmp;
        opts.pattern = c.pattern;
        opts.recursive = true;
        opts.use_regex = true;
        opts.ignore_case = c.ignore_case;
        opts.force_realtime = false;
        auto idx_results = idx.search(c.pattern, opts);

        auto rt_results =
            realtime_search(tmp, c.pattern, ext_set, true, c.ignore_case);

        auto idx_keys = to_keys(idx_results);
        auto rt_keys = to_keys(rt_results);
        if (idx_keys != rt_keys) {
            throw ::simpletest::TestFailure(
                std::string("regex matrix mismatch for pattern: ") +
                wstring_to_utf8(c.pattern));
        }
    }

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}

// ============================================================
// 测试 21：B1 —— UTF-16 大文件建索引流式化回归
// 旧实现把 UTF-16 大文件排除在流式路径外（回退整文件 read_text，内存路径
// 有 OOM 残留）。B1 起 for_each_line_mmap 支持码元级切行。用「注入 4KB 小
// 窗口」强制多窗口滑动 + 码元边界拆分，LE/BE 各验证一遍：流式召回 ==
// 内存路径召回 == 写入行数。
// ============================================================

static void write_utf16_file(const std::filesystem::path& path,
                             const std::wstring& content, bool big_endian) {
    std::vector<unsigned char> bytes;
    if (big_endian) { bytes.push_back(0xFE); bytes.push_back(0xFF); }
    else            { bytes.push_back(0xFF); bytes.push_back(0xFE); }
    for (wchar_t ch : content) {
        unsigned lo = ch & 0xFF, hi = (ch >> 8) & 0xFF;
        if (big_endian) { bytes.push_back(static_cast<unsigned char>(hi));
                          bytes.push_back(static_cast<unsigned char>(lo)); }
        else            { bytes.push_back(static_cast<unsigned char>(lo));
                          bytes.push_back(static_cast<unsigned char>(hi)); }
    }
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

TEST_CASE(indexer_utf16_large_file_streaming) {
    const std::wstring MARKER = L"ZzzUtf16Marker";
    const size_t kMarkerLines = 300;
    const size_t kTotalLines = 20000;   // ~1.2MB UTF-16 字节，超过 1MB 阈值

    std::wstring content;
    for (size_t i = 0; i < kTotalLines; ++i) {
        if (i < kMarkerLines) content += MARKER;
        content += L" 填充行 payload line number " + std::to_wstring(i);
        content += L"\n";
    }

    for (int be = 0; be <= 1; ++be) {
        const wchar_t* suffix = be ? L"be" : L"le";
        // 流式路径：阈值 1MB + 注入 4KB 窗口（多窗口滑动/码元边界全覆盖）
        auto dir_s = make_test_dir(std::wstring(L"textsearch_idx_u16") + suffix + L"_stream");
        write_utf16_file(dir_s / L"big.txt", content, be != 0);
        Indexer idx_s;
        idx_s.set_bigfile_threshold_mb(1);
        set_index_mmap_window(4096);
        std::set<std::wstring> ext_set = {L".txt"};
        REQUIRE(idx_s.build(dir_s, ext_set));
        SearchOptions opts;
        opts.root = dir_s;
        opts.recursive = true;
        size_t n_stream = idx_s.search(MARKER, opts).size();

        // 内存路径：阈值 100GB → read_text 整文件（口径交叉校验基准）
        auto dir_m = make_test_dir(std::wstring(L"textsearch_idx_u16") + suffix + L"_mem");
        write_utf16_file(dir_m / L"big.txt", content, be != 0);
        Indexer idx_m;
        idx_m.set_bigfile_threshold_mb(100000);
        REQUIRE(idx_m.build(dir_m, ext_set));
        SearchOptions opts_m;
        opts_m.root = dir_m;
        opts_m.recursive = true;
        size_t n_mem = idx_m.search(MARKER, opts_m).size();

        REQUIRE_EQ(n_stream, kMarkerLines);   // 流式路径召回正确（未 OOM 崩溃）
        REQUIRE_EQ(n_mem, kMarkerLines);      // 内存路径召回正确
        REQUIRE_EQ(n_stream, n_mem);          // 两条路径口径一致

        std::error_code ec;
        std::filesystem::remove_all(dir_s, ec);
        std::filesystem::remove_all(dir_m, ec);
    }
    set_index_mmap_window(64ULL * 1024 * 1024);   // 恢复默认窗口
}

// ============================================================
// 测试 22：B3 —— build() 进度回调
// 初始一次 (0,total)，之后每完成一个文件上报一次，(total,total) 必达。
// 并行 worker 会并发调用回调（见 indexer.h BuildProgress 注释）——
// 回调用 mutex 计数，同时示范线程安全的接入方式。
// ============================================================

TEST_CASE(indexer_build_progress_callback) {
    auto tmp = make_test_dir(L"textsearch_idx_build_progress");
    for (int i = 0; i < 30; ++i) {
        write_utf8_file(tmp / (L"f" + std::to_wstring(i) + L".txt"),
                        L"alpha beta gamma line " + std::to_wstring(i) + L"\n");
    }
    std::set<std::wstring> ext_set = {L".txt"};

    std::mutex mu;
    size_t calls = 0;
    size_t last_total = 0;
    bool saw_final = false;
    bool saw_initial = false;
    {
        Indexer idx;
        bool ok = idx.build(tmp, ext_set, [&](size_t done, size_t total) {
            std::lock_guard<std::mutex> lk(mu);
            ++calls;
            last_total = total;
            if (done == 0) saw_initial = true;
            if (total > 0 && done == total) saw_final = true;
        });
        REQUIRE(ok);
    }

    REQUIRE(saw_initial);                 // 枚举完成后立即 (0, total)
    REQUIRE(saw_final);                   // 全部完成时到达 (total, total)
    REQUIRE_EQ(last_total, size_t(30));
    REQUIRE(calls >= 31);                 // 初始 1 次 + 每文件至少 1 次

    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
}
