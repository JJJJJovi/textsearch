// tests/test_query_cache.cpp — A2 查询结果 LRU 缓存单测（TODO 2026-10-27 验收单）
// 覆盖：
//   ① 同参二连发第二次 <1ms（同进程，走真实 Indexer::search 接线）
//   ② 键字段逐一变化必须 miss（pattern/ignore_case/whole_word/use_regex/
//      recursive/ext_set/root/mtime/size/content_version）
//   ③ 增量更新后必须 miss（且结果反映新内容，不得返回陈旧缓存）
//   ④ 达 max_results 上限不缓存
//   ⑤ B3 告警路径不缓存（用户勾选了索引内不存在的扩展名）
//   ⑥ LRU 淘汰与 touch 刷新（独立组件级）
// 反向验证：临时破坏 Key::operator== 的字段完整性（如去掉 ext_set_joined）
//   → key_sensitivity 与 key_fields_miss 必须失败（记录于 HANDOFF）。
#include "simpletest.h"
#include "core/query_cache.h"
#include "core/indexer.h"
#include "core/types.h"
#include "core/util.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

// ---- 辅助：写 UTF-8 文件 ----
static void qcache_write_utf8(const std::filesystem::path& path,
                              const std::wstring& content) {
    std::string utf8 = wstring_to_utf8(content);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

// ---- 辅助：创建测试目录 ----
static std::filesystem::path qcache_make_dir(const std::wstring& name) {
    auto tmp = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    return tmp;
}

// ---- 辅助：结果比较键（(path,line,col) 全集比较）----
static std::set<std::wstring> qcache_result_keys(
    const std::vector<MatchResult>& r) {
    std::set<std::wstring> keys;
    for (const auto& m : r) {
        keys.insert(m.file_path.wstring() + L":" +
                    std::to_wstring(m.line_no) + L":" +
                    std::to_wstring(m.col));
    }
    return keys;
}

// ---- 辅助：构造一条 MatchResult ----
static MatchResult qcache_make_match(int line, const std::wstring& text) {
    MatchResult m;
    m.line_no = line;
    m.col = 1;
    m.line_text = text;
    m.file_path = L"C:\\fake\\file.txt";
    return m;
}

// ---- 辅助：索引搜索（真实接线，一次 open 多次 search）----
static std::vector<MatchResult> qcache_search(
    Indexer& idx, const std::wstring& pattern,
    bool ignore_case = false, bool whole_word = false, bool use_regex = false,
    const std::set<std::wstring>& ext_set = {}, size_t max_results = 100000) {
    SearchOptions opts;
    opts.pattern = pattern;
    opts.recursive = true;
    opts.use_regex = use_regex;
    opts.ignore_case = ignore_case;
    opts.whole_word = whole_word;
    opts.max_results = max_results;
    return idx.search(pattern, opts, ext_set);
}

// ============================================================
// 组件级：LRU 淘汰 + touch 刷新 + 计数
// ============================================================

TEST_CASE(query_cache_lru_eviction_and_touch) {
    QueryCache c(3);
    auto key = [](const std::wstring& p) {
        QueryCache::Key k;
        k.pattern = p;
        return k;
    };

    c.put(key(L"a"), {qcache_make_match(1, L"a1")});
    c.put(key(L"b"), {qcache_make_match(2, L"b1")});
    c.put(key(L"c"), {qcache_make_match(3, L"c1")});
    REQUIRE_EQ(c.size(), size_t{3});

    // touch a → a 变最新，b 成为最久未用
    std::vector<MatchResult> out;
    REQUIRE(c.get(key(L"a"), out));
    REQUIRE_EQ(out.size(), size_t{1});
    REQUIRE_EQ(out[0].line_text, std::wstring(L"a1"));

    // 放入 d → 淘汰 b（而非 a）
    c.put(key(L"d"), {qcache_make_match(4, L"d1")});
    REQUIRE_EQ(c.size(), size_t{3});
    REQUIRE(c.get(key(L"a"), out));          // a 仍在（刚被 touch）
    REQUIRE(!c.get(key(L"b"), out));         // b 已被淘汰
    REQUIRE(c.get(key(L"c"), out));
    REQUIRE(c.get(key(L"d"), out));

    // 同键 put 覆盖且不增容量
    c.put(key(L"d"), {qcache_make_match(44, L"d2")});
    REQUIRE_EQ(c.size(), size_t{3});
    REQUIRE(c.get(key(L"d"), out));
    REQUIRE_EQ(out[0].line_text, std::wstring(L"d2"));

    // 计数：get 序列 a; a,c,d; d → 5 次命中 + 1 次 miss（b）
    REQUIRE_EQ(c.hits(), uint64_t{5});
    REQUIRE_EQ(c.misses(), uint64_t{1});

    c.clear();
    REQUIRE_EQ(c.size(), size_t{0});
    REQUIRE(!c.get(key(L"a"), out));
}

// ============================================================
// 组件级：键字段逐一敏感（②的键完整性基础；反向验证的靶子）
// ============================================================

TEST_CASE(query_cache_key_sensitivity) {
    auto base = []() {
        QueryCache::Key k;
        k.pattern = L"alpha";
        k.use_regex = false;
        k.ignore_case = false;
        k.whole_word = false;
        k.recursive = true;
        k.root = L"C:\\root";
        k.ext_set_joined = L".txt\x1F";
        k.index_mtime = 1000;
        k.index_size = 2000;
        k.content_version = 3;
        return k;
    };
    QueryCache c(8);
    std::vector<MatchResult> out;

    // 基准键命中
    c.put(base(), {qcache_make_match(1, L"m")});
    REQUIRE(c.get(base(), out));

    // 逐一变化每个字段 → 必须 miss
    auto expect_miss_on = [&](const char* what, QueryCache::Key k) {
        if (c.get(k, out)) {
            throw ::simpletest::TestFailure(
                std::string("键字段变化未产生 miss（陈旧结果风险）: ") + what);
        }
    };
    { QueryCache::Key k = base(); k.pattern = L"alphb"; expect_miss_on("pattern", k); }
    { QueryCache::Key k = base(); k.use_regex = true; expect_miss_on("use_regex", k); }
    { QueryCache::Key k = base(); k.ignore_case = true; expect_miss_on("ignore_case", k); }
    { QueryCache::Key k = base(); k.whole_word = true; expect_miss_on("whole_word", k); }
    { QueryCache::Key k = base(); k.recursive = false; expect_miss_on("recursive", k); }
    { QueryCache::Key k = base(); k.root = L"C:\\root2"; expect_miss_on("root", k); }
    { QueryCache::Key k = base(); k.ext_set_joined = L".md\x1F"; expect_miss_on("ext_set", k); }
    { QueryCache::Key k = base(); k.ext_set_joined = L".txt"; expect_miss_on("ext_set拼接歧义", k); }
    { QueryCache::Key k = base(); k.index_mtime = 1001; expect_miss_on("index_mtime", k); }
    { QueryCache::Key k = base(); k.index_size = 2001; expect_miss_on("index_size", k); }
    { QueryCache::Key k = base(); k.content_version = 4; expect_miss_on("content_version", k); }
}

// ============================================================
// 接线级：①同参二连发第二次 <1ms + 命中结果一致
// ============================================================

TEST_CASE(indexer_query_cache_second_call_fast) {
    auto root = qcache_make_dir(L"textsearch_qcache_fast");
    // 语料：12 文件 × 3000 行，每 100 行一个 needle（共 360 条结果）。
    // 全量搜索需读入全部 12 个文件（~2.4MB），远超 1ms —— 「<1ms」断言才有
    // 判别力；结果数取小是因为命中路径要拷贝整份结果（360 条 ≈0.15ms，
    // CI 慢机仍有数倍余量），3600 条时拷贝本身就会破 1ms（实测 1.6ms）。
    std::wstring body;
    body.reserve(3000 * 40);
    for (int i = 1; i <= 3000; ++i) {
        body += L"line content for benchmark row ";
        body += std::to_wstring(i);
        if (i % 100 == 0) body += L" needle here";
        body += L"\n";
    }
    std::set<std::wstring> exts{L".txt"};
    for (int f = 0; f < 12; ++f) {
        qcache_write_utf8(root / (L"f" + std::to_wstring(f) + L".txt"), body);
    }

    global_query_cache().clear();
    global_query_cache().reset_stats();

    Indexer idx;
    REQUIRE(idx.build(root, exts));
    REQUIRE(idx.open(root));

    uint64_t m0 = global_query_cache().misses();
    auto t0 = std::chrono::steady_clock::now();
    std::vector<MatchResult> first = qcache_search(idx, L"needle");
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(!first.empty());

    uint64_t h0 = global_query_cache().hits();
    auto t2 = std::chrono::steady_clock::now();
    std::vector<MatchResult> second = qcache_search(idx, L"needle");
    auto t3 = std::chrono::steady_clock::now();

    // ①命中：hits 恰好 +1（确定性证明走的是缓存，而非碰巧快）
    REQUIRE_EQ(global_query_cache().hits(), h0 + 1);
    // ①性能：第二次 <1ms，且首查 > 3× 二查（缓存失效时两者同为全量搜索，
    // 比值断言必失败 —— 自证本测试的 <1ms 有判别力）
    auto first_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    auto second_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
    if (second_us >= 1000) {
        throw ::simpletest::TestFailure(
            "同参二连发第二次耗时 " + std::to_string(second_us) +
            "us，未达 <1ms 验收线");
    }
    if (first_us <= second_us * 3) {
        throw ::simpletest::TestFailure(
            "首查 " + std::to_string(first_us) + "us 未显著慢于二查 " +
            std::to_string(second_us) + "us —— 缓存疑似未生效");
    }
    // ①正确：两次结果一致
    REQUIRE_EQ(second.size(), first.size());
    REQUIRE(qcache_result_keys(second) == qcache_result_keys(first));

    // 结果数符合预期：每文件 30 行 needle → 12 × 30 = 360
    REQUIRE_EQ(first.size(), size_t{360});
    (void)m0;
}

// ============================================================
// 接线级：②键字段逐一变化必须 miss（走 Indexer::search 真实键）
// ============================================================

TEST_CASE(indexer_query_cache_key_fields_miss) {
    auto root = qcache_make_dir(L"textsearch_qcache_keys");
    qcache_write_utf8(root / L"a.txt", L"alpha beta\ngamma alpha\n");
    qcache_write_utf8(root / L"b.txt", L"ALPHA delta\nepsilon\n");
    std::set<std::wstring> exts{L".txt"};

    global_query_cache().clear();
    global_query_cache().reset_stats();

    Indexer idx;
    REQUIRE(idx.build(root, exts));
    REQUIRE(idx.open(root));

    uint64_t h = global_query_cache().hits();
    // 基准查询两次：第二次必须命中（确认缓存已填充）
    std::vector<MatchResult> r1 = qcache_search(idx, L"alpha");
    std::vector<MatchResult> r2 = qcache_search(idx, L"alpha");
    REQUIRE_EQ(global_query_cache().hits(), h + 1);
    REQUIRE_EQ(r2.size(), r1.size());

    // 逐一变化键字段 → 每次都必须 miss（hits 不再增长）
    auto expect_miss = [&](const char* what, auto&& fn) {
        uint64_t before = global_query_cache().hits();
        auto r = fn();
        (void)r;
        if (global_query_cache().hits() != before) {
            throw ::simpletest::TestFailure(
                std::string("变化 ") + what + " 后意外命中缓存（键不完整或未参与）");
        }
    };
    expect_miss("pattern", [&] { return qcache_search(idx, L"beta"); });
    expect_miss("ignore_case", [&] { return qcache_search(idx, L"alpha", true); });
    expect_miss("whole_word", [&] { return qcache_search(idx, L"alpha", false, true); });
    expect_miss("use_regex", [&] { return qcache_search(idx, L"alp.a", false, false, true); });
    expect_miss("ext_set", [&] { return qcache_search(idx, L"alpha", false, false, false, {L".md"}); });
    // 变化后基准查询仍命中（缓存条目未被破坏）
    REQUIRE_EQ(global_query_cache().hits(), h + 1);
    std::vector<MatchResult> r3 = qcache_search(idx, L"alpha");
    REQUIRE_EQ(global_query_cache().hits(), h + 2);
    REQUIRE_EQ(r3.size(), r1.size());
}

// ============================================================
// 接线级：③增量更新后必须 miss，且结果反映新内容
// ============================================================

TEST_CASE(indexer_query_cache_incremental_miss) {
    auto root = qcache_make_dir(L"textsearch_qcache_incr");
    qcache_write_utf8(root / L"x.txt", L"freshmarker one\nplain line\n");
    std::set<std::wstring> exts{L".txt"};

    global_query_cache().clear();
    global_query_cache().reset_stats();

    Indexer idx;
    REQUIRE(idx.build(root, exts));
    REQUIRE(idx.open(root));

    std::vector<MatchResult> r1 = qcache_search(idx, L"freshmarker");
    REQUIRE_EQ(r1.size(), size_t{1});
    REQUIRE_EQ(r1[0].line_text, std::wstring(L"freshmarker one"));

    // 修改文件：追加含同词的新行 → 增量更新 → 同参查询必须 miss 且看到新行
    qcache_write_utf8(root / L"x.txt",
                      L"freshmarker one\nplain line\nfreshmarker two\n");
    size_t changed = 0;
    REQUIRE(idx.update_incremental(&changed));
    REQUIRE(changed >= size_t{1});

    uint64_t h = global_query_cache().hits();
    std::vector<MatchResult> r2 = qcache_search(idx, L"freshmarker");
    REQUIRE_EQ(global_query_cache().hits(), h);   // 必须 miss
    REQUIRE_EQ(r2.size(), size_t{2});             // 且是新内容（非陈旧缓存）
    REQUIRE(qcache_result_keys(r2).count(root.wstring() + L"\\x.txt:3:1") == 1);
}

// ============================================================
// 接线级：④达 max_results 上限不缓存
// ============================================================

TEST_CASE(indexer_query_cache_no_cache_when_capped) {
    auto root = qcache_make_dir(L"textsearch_qcache_cap");
    std::wstring body;
    for (int i = 0; i < 5; ++i) {
        body += L"cappedmarker line ";
        body += std::to_wstring(i);
        body += L"\n";
    }
    qcache_write_utf8(root / L"c.txt", body);
    std::set<std::wstring> exts{L".txt"};

    global_query_cache().clear();
    global_query_cache().reset_stats();

    Indexer idx;
    REQUIRE(idx.build(root, exts));
    REQUIRE(idx.open(root));

    // max_results=2 < 5 条命中 → 截断 → 两次同参查询都必须 miss
    std::vector<MatchResult> r1 = qcache_search(idx, L"cappedmarker", false, false, false, {}, 2);
    REQUIRE_EQ(r1.size(), size_t{2});
    uint64_t h = global_query_cache().hits();
    std::vector<MatchResult> r2 = qcache_search(idx, L"cappedmarker", false, false, false, {}, 2);
    REQUIRE_EQ(r2.size(), size_t{2});
    REQUIRE_EQ(global_query_cache().hits(), h);   // 未被缓存

    // 对照：放宽上限后同参查询可缓存（证明上面 miss 是上限守卫而非键问题）
    std::vector<MatchResult> r3 = qcache_search(idx, L"cappedmarker", false, false, false, {}, 100000);
    REQUIRE_EQ(r3.size(), size_t{5});
    REQUIRE_EQ(global_query_cache().hits(), h);   // 新键首查 miss
    std::vector<MatchResult> r4 = qcache_search(idx, L"cappedmarker", false, false, false, {}, 100000);
    REQUIRE_EQ(r4.size(), size_t{5});
    REQUIRE_EQ(global_query_cache().hits(), h + 1);
}

// ============================================================
// 接线级：⑤B3 告警路径不缓存（勾选了索引内不存在的扩展名）
// ============================================================

TEST_CASE(indexer_query_cache_no_cache_b3) {
    auto root = qcache_make_dir(L"textsearch_qcache_b3");
    qcache_write_utf8(root / L"t.txt", L"b3marker inside\n");
    qcache_write_utf8(root / L"l.log", L"b3marker in log\n");   // 不会入索引
    std::set<std::wstring> exts{L".txt"};

    global_query_cache().clear();
    global_query_cache().reset_stats();

    Indexer idx;
    REQUIRE(idx.build(root, exts));
    REQUIRE(idx.open(root));

    // 勾选含索引外扩展名（.log 不在 meta.ext_set）→ B3 路径 → 不缓存
    std::set<std::wstring> user_exts{L".txt", L".log"};
    std::vector<MatchResult> r1 =
        qcache_search(idx, L"b3marker", false, false, false, user_exts);
    REQUIRE_EQ(r1.size(), size_t{1});   // 只有 .txt 那条
    uint64_t h = global_query_cache().hits();
    std::vector<MatchResult> r2 =
        qcache_search(idx, L"b3marker", false, false, false, user_exts);
    REQUIRE_EQ(r2.size(), size_t{1});
    REQUIRE_EQ(global_query_cache().hits(), h);   // B3 路径未写缓存

    // 对照：仅勾选索引内类型 → 正常缓存
    std::vector<MatchResult> r3 =
        qcache_search(idx, L"b3marker", false, false, false, {L".txt"});
    REQUIRE_EQ(r3.size(), size_t{1});
    std::vector<MatchResult> r4 =
        qcache_search(idx, L"b3marker", false, false, false, {L".txt"});
    REQUIRE_EQ(r4.size(), size_t{1});
    REQUIRE_EQ(global_query_cache().hits(), h + 1);
}
