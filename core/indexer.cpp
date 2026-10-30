// core/indexer.cpp — 倒排索引完整实现（T4b）
// 使用 IndexStore（简易文件式 KV 存储）替代 LMDB，实现倒排索引。
// 支持：子串搜索（bi-gram + 邻接验证 F.2.2）、正则搜索（trigram 加速 E.3）、
//       增量更新（mtime/size 对比）、损坏恢复（F.2.5 checksum）。
#include "core/indexer.h"
#include "core/encoding.h"
#include "core/filesystem.h"
#include "core/logger.h"
#include "core/query_cache.h"
#include "core/matcher.h"
#include "core/text_detect.h"
#include "core/threadpool.h"
#include "core/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "core/port/port_mmap.h"

// ---- 索引内容版本（meta.version）----
// 与 mmap_index.cpp 的文件格式版本（magic 后的 version=2）是两回事：
// 这里管的是"索引里存了什么内容"。内容口径变化时 +1，旧索引会被
// index_exists() 判为不存在，从而触发重建，避免用户拿着内容过期的
// 索引继续得到漏报结果且毫不知情。
//
// v1 → v2（2026-08-31）：tokenize_line 增加下划线子词补记。
//    旧索引的 postings 缺 mmap_index / file_mtime_unix 这类连写标识符的
//    子词，搜 "mmap"、"mtime" 时整行漏报。
// v2 → v3（2026-09-01）：tokenize_line 为长度 >= 4 的 ASCII token 补记
//    3-gram 子词（带各自列号）。
//    下划线子词只覆盖以 '_' 分隔的连写，覆盖不到"既无分隔符、也无大小写
//    边界"的内部子串。真实语料实测：搜 "RDP" 时 wizardp 里的 rdp 全部漏报
//    （索引 0 条 / 实时 12 条）；搜 "mmap" 时 MmapIndexReader 里的 mmap
//    漏报。一个 token 的全部 3-gram 覆盖其内部任意长度 >= 3 的连续子串，
//    配合 search_substring 把查询词同样展开成 3-gram + 邻接验证，即可零漏报。
//    注意：postings 仍是 (file_id, line, col) 位置级三元组，命中即可定位到
//    列，高亮一次扫描够用 —— 此处刻意不做行级（行级会让高亮退化成二次扫描）。
static constexpr uint32_t kIndexContentVersion = 3;

// ---- index_exists 进程内 mtime/size 缓存 ----
// verify() 要读全索引文件算 FNV-1a checksum，大索引耗时显著；同一文件
// mtime+size 未变时直接复用上次结果。缓存仅进程内有效，跨进程安全
//（文件被外部修改/删除/重建，mtime 变化即 cache miss）。
namespace {

// 读取 meta.version（little-endian u32）。无版本标记或长度不足 → 返回 0
inline uint32_t read_meta_version(const IndexStore& store) {
    const std::vector<uint8_t>* v = store.get("meta", "version");
    if (!v || v->size() < 4) return 0;
    return static_cast<uint32_t>((*v)[0])
         | (static_cast<uint32_t>((*v)[1]) << 8)
         | (static_cast<uint32_t>((*v)[2]) << 16)
         | (static_cast<uint32_t>((*v)[3]) << 24);
}

// 找最后一个 \n 的字节位置（无则 npos）。供大文件 MMF 滑动窗口在 \n 边界切行，
// 避免 UTF-8/GBK 多字节字符跨窗口被拆断（0x0A 在两种编码里都是单字节 \n，R6/R15）。
inline size_t find_last_newline(const char* p, size_t n) {
    for (size_t i = n; i > 0; --i) {
        if (p[i - 1] == '\n') return i - 1;
    }
    return std::string::npos;
}

// 大文件流式索引：postings 累加器 flush 阈值（条数）。每次达到即把内存压回 store，
// 内存有界（~500k 条 ≈ 数十 MB，远低于整文件读入的 2× 文件大小），杜绝 OOM。
static constexpr size_t kPostingFlushCap = 500'000;

// ---- varint + zigzag（posting list / file_id 集合压缩编码）----
// 小整数 1 字节、至多 5 字节。配合 delta：posting list 中 file_id/line/col 多为
// 小值且相邻项差值小，绝大多数 1 字节，整体比裸 uint32 三元组（12B/项）省 60-75%。
inline void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) { out.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u)); v >>= 7; }
    out.push_back(static_cast<uint8_t>(v));
}
// 裸指针版（mmap 零拷贝路径用）：data/len 为缓冲区，pos 为读写位置
inline bool get_varint_p(const uint8_t* data, size_t len, size_t& pos, uint64_t& out) {
    out = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {  // uint64 至多 10 字节
        if (pos >= len) return false;
        uint8_t b = data[pos++];
        out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) return true;
        shift += 7;
    }
    return false;  // 溢出 / 格式错误
}
// vector 包装（内存模式 / 老代码兼容）
inline bool get_varint(const std::vector<uint8_t>& data, size_t& pos, uint64_t& out) {
    return get_varint_p(data.data(), data.size(), pos, out);
}
inline uint32_t zig_enc(int32_t n) {
    uint32_t u = static_cast<uint32_t>(n);
    return (u << 1) ^ (-(u >> 31));   // 负数掩码 0xFFFFFFFF，正数 0
}
inline int32_t  zig_dec(uint32_t n) { return static_cast<int32_t>((n >> 1) ^ (-(static_cast<int32_t>(n & 1u)))); }

// mtime → unix 秒统一用 core/util.h 的 file_mtime_unix（确定性转换）。
// 索引侧 build / index_single_file / update_incremental 与扫描侧 mtime_filter 必须共用同一函数，
// 否则两侧算出的秒值可能差 1，导致「刚重建的文件仍被判为已修改」→ 每次都无谓全量重写。

struct IndexExistsCacheEntry {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = static_cast<std::uintmax_t>(-1);
    bool exists = false;
};
struct IndexExistsCache {
    std::mutex mtx;
    std::unordered_map<std::wstring, IndexExistsCacheEntry> map;
};
IndexExistsCache& index_exists_cache() {
    static IndexExistsCache inst;
    return inst;
}
// 路径归一化为 key（Windows 大小写不敏感）
std::wstring index_cache_key(const std::filesystem::path& root) {
    std::wstring s = root.wstring();
    for (auto& c : s) c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
    return s;
}
} // namespace

// ---- 构造 / 析构 ----

Indexer::Indexer() = default;
Indexer::~Indexer() = default;

// ---- 索引路径计算 ----

std::filesystem::path Indexer::compute_index_path(
    const std::filesystem::path& root) const {
    // <root>/.textsearch/index.lmdb
    return root / ".textsearch" / "index.lmdb";
}

// ---- CJK 判定 ----

bool Indexer::is_cjk(wchar_t c) const {
    // CJK 统一表意文字（基本区 + 扩展A + 兼容区）
    return (c >= 0x4E00 && c <= 0x9FFF) ||  // CJK Unified Ideographs
           (c >= 0x3400 && c <= 0x4DBF) ||  // CJK Extension A
           (c >= 0xF900 && c <= 0xFAFF);    // CJK Compatibility Ideographs
}

// ---- 编码映射 ----

uint8_t Indexer::encoding_to_uint8(Encoding enc) const {
    switch (enc) {
        case Encoding::Utf8:    return 0;
        case Encoding::Gbk:     return 1;
        case Encoding::Utf16Le:return 2;
        case Encoding::Utf16Be:return 3;
        default:                return 0;
    }
}

Encoding Indexer::uint8_to_encoding(uint8_t v) const {
    switch (v) {
        case 0:  return Encoding::Utf8;
        case 1:  return Encoding::Gbk;
        case 2:  return Encoding::Utf16Le;
        case 3:  return Encoding::Utf16Be;
        default: return Encoding::Unknown;
    }
}

// ---- 词项切分 ----

std::vector<std::pair<std::wstring, int>>
Indexer::tokenize_line(const std::wstring& line) const {
    std::vector<std::pair<std::wstring, int>> tokens;
    size_t i = 0;
    while (i < line.size()) {
        wchar_t c = line[i];
        if (is_word_char(c)) {
            // ASCII 标识符 [A-Za-z0-9_]+
            size_t start = i;
            while (i < line.size() && is_word_char(line[i])) ++i;
            std::wstring term = line.substr(start, i - start);
            tokens.push_back({term, static_cast<int>(start)});

            // 额外补记「下划线子词」——零漏报的关键修复
            //
            // 2026-08-31 会话检索实验发现：is_word_char 把 '_' 视为词内字符，
            // 所以 mmap_index.cpp 整体是一个 term，postings["mmap"] 里没有它。
            // 子串搜索查 postings["mmap"] 时，该行连候选都进不去 → 整行漏报。
            // 实测：01f1514f.jsonl 中「mmap」漏 8/23 条、「mtime」漏 5/122 条，
            // 全部落在只以 mmap_index / file_mtime_unix 形式出现的行上；
            // 而同一行只要有一处独立成词，整行的所有匹配就都回来了。
            //
            // 做法是"只额外补记子词、保留整词 term"，因此：
            //   搜 "mmap"       → 命中 mmap_index 的贡献（本次修复）
            //   搜 "mmap_index" → 仍走整词 term，语义不变
            //   全词匹配（whole_word）语义不变（整词 term 仍在）
            if (term.find(L'_') != std::wstring::npos) {
                size_t s = 0;
                while (s < term.size()) {
                    while (s < term.size() && term[s] == L'_') ++s;
                    size_t e = s;
                    while (e < term.size() && term[e] != L'_') ++e;
                    if (e > s) {
                        tokens.push_back({term.substr(s, e - s),
                                          static_cast<int>(start + s)});
                    }
                    s = e;
                }
            }

            // v3 子串友好：补记 ASCII token 的全部 3-gram（带各自列号）
            //
            // 只补长度 >= 4 的 token：长度 == 3 的 token，其唯一 3-gram 就是
            // 整词本身，整词 term 已覆盖（不再补，避免重复 posting 白占空间）。
            //
            // 一个 token 的全部 3-gram 覆盖它内部任意长度 >= 3 的连续子串，
            // 因此查询侧把查询词同样展开成 3-gram 再做邻接验证，即可召回
            // "查询词是长 token 内部子串"的命中（索引期/查询期口径必须一致）：
            //   rdp  ⊂ wizardp          —— 真实语料实测 索引 0 → 实时 12
            //   mmap ⊂ MmapIndexReader  —— 会话语料同类漏报
            // postings 仍保留 (file_id, line, col)：定位到列，高亮一次扫描。
            if (term.size() >= 4) {
                for (size_t k = 0; k + 3 <= term.size(); ++k) {
                    tokens.push_back({term.substr(k, 3),
                                      static_cast<int>(start + k)});
                }
            }
        } else if (is_cjk(c)) {
            // CJK bi-gram：收集连续 CJK 字符，两两组合
            size_t start = i;
            while (i < line.size() && is_cjk(line[i])) ++i;
            std::wstring cjk_run = line.substr(start, i - start);
            for (size_t j = 0; j + 1 < cjk_run.size(); ++j) {
                std::wstring bg = cjk_run.substr(j, 2);
                tokens.push_back({bg, static_cast<int>(start + j)});
            }
            // 单个 CJK 字符（无法形成 bi-gram）→ 不产生 token
        } else {
            // 标点 / 空白 → 分隔符
            ++i;
        }
    }
    return tokens;
}

std::vector<std::wstring> Indexer::bigram_cjk(const std::wstring& s) const {
    std::vector<std::wstring> result;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        result.push_back(s.substr(i, 2));
    }
    return result;
}

std::vector<Indexer::PatternToken>
Indexer::tokenize_pattern(const std::wstring& pattern) const {
    std::vector<PatternToken> tokens;
    size_t i = 0;
    while (i < pattern.size()) {
        wchar_t c = pattern[i];
        if (is_word_char(c)) {
            size_t start = i;
            while (i < pattern.size() && is_word_char(pattern[i])) ++i;
            std::wstring term = pattern.substr(start, i - start);
            tokens.push_back({term, static_cast<int>(start),
                              static_cast<int>(term.size())});
        } else if (is_cjk(c)) {
            size_t start = i;
            while (i < pattern.size() && is_cjk(pattern[i])) ++i;
            std::wstring cjk_run = pattern.substr(start, i - start);
            for (size_t j = 0; j + 1 < cjk_run.size(); ++j) {
                std::wstring bg = cjk_run.substr(j, 2);
                tokens.push_back({bg, static_cast<int>(start + j), 2});
            }
        } else {
            ++i;
        }
    }
    return tokens;
}

// ---- trigram 提取 ----

std::set<std::string> Indexer::extract_trigrams(const std::wstring& line,
                                                 bool ignore_case) const {
    std::set<std::string> grams;
    std::wstring s = ignore_case ? to_lower_wstring(line) : line;
    // 字符级 3-gram：i + 3 <= s.size()
    for (size_t i = 0; i + 3 <= s.size(); ++i) {
        std::wstring tri = s.substr(i, 3);
        grams.insert(wstring_to_utf8(tri));
    }
    return grams;
}

// ---- 正则字面量段提取 ----
//
// 返回按顶层 '|' 分支分组的字面量段（每段长度≥3）。
// 候选集语义：某分支的候选 = 组内各 run 候选的交集；总候选 = 各分支并集。
// 正确性要点（零漏报，候选只能变宽不能变窄）：
//   - "a|b" 两支各自必选，整体取并集（旧实现按交集 → alternation 漏报）；
//   - "(x|y)" 组内可选 → 整组 run 丢弃；
//   - "(ab)?" / "(ab)*" / "(ab){0,n}" 组整体可选 → 整组 run 丢弃；
//   - 字面字符后跟 '?' / '*' / '{0,n}' → 该字符可选，从 run 尾部剔除；
//   - 字符类 "[...]" 内不是字面量 → 整段跳过；
//   - {m,n} 内容（数字/逗号）不是字面量 → 跳过。
std::vector<std::vector<std::wstring>>
Indexer::extract_literal_runs(const std::wstring& pattern) const {
    std::vector<std::vector<std::wstring>> groups;
    std::vector<std::wstring> cur_alt;  // 当前顶层分支的 runs
    std::wstring cur;                   // 正在累积的字面段

    struct GroupLevel {
        std::vector<std::wstring> runs;  // 组内（更深顶层分支之外）的 runs
        bool discard = false;            // 组内含 '|' 等 → 整组放弃筛选
    };
    std::vector<GroupLevel> stack;

    auto dest = [&]() -> std::vector<std::wstring>& {
        return stack.empty() ? cur_alt : stack.back().runs;
    };
    auto flush = [&]() {
        if (cur.size() >= 3) dest().push_back(cur);
        cur.clear();
    };
    // pattern[pos] == L'{'，判断量词最小次数是否为 0（无法解析按 0 保守处理）
    auto brace_min_zero = [&](size_t pos) {
        size_t j = pos + 1;
        std::wstring num;
        while (j < pattern.size() && iswdigit(pattern[j])) {
            num.push_back(pattern[j]);
            ++j;
        }
        if (j >= pattern.size() || (pattern[j] != L',' && pattern[j] != L'}'))
            return true;  // 非法形态 → 保守当可选
        return num.empty() || num == L"0";  // {,n} 即 {0,n}
    };

    for (size_t i = 0; i < pattern.size(); ++i) {
        wchar_t c = pattern[i];
        if (c == L'\\') {
            if (i + 1 < pattern.size()) {
                wchar_t next = pattern[i + 1];
                // 非字面量转义（字符类简写、断言、反向引用等）
                if (next == L'd' || next == L'w' || next == L's' ||
                    next == L'D' || next == L'W' || next == L'S' ||
                    next == L'b' || next == L'B' || next == L'n' ||
                    next == L'r' || next == L't' || next == L'v' ||
                    next == L'f' || next == L'a' || next == L'z' ||
                    next == L'A' || next == L'Z' || next == L'G' ||
                    next == L'k' || next == L'P' || next == L'p' ||
                    iswdigit(next)) {
                    flush();
                    ++i;
                } else {
                    // \x, \., \*, 等 → 字面量
                    cur.push_back(next);
                    ++i;
                }
            }
            continue;
        }
        if (c == L'[') {
            // 字符类整体跳过（类内不是字面量；类前字面段照常保留）
            flush();
            ++i;
            while (i < pattern.size() && pattern[i] != L']') {
                if (pattern[i] == L'\\') ++i;
                ++i;
            }
            continue;
        }
        if (c == L'(') {
            flush();
            stack.push_back(GroupLevel{});
            // "(?:" / "(?=" / "(?!" / "(?<name>" 前缀处理
            if (i + 1 < pattern.size() && pattern[i + 1] == L'?') {
                wchar_t f = (i + 2 < pattern.size()) ? pattern[i + 2] : 0;
                if (f == L'=' || f == L'!') {
                    stack.back().discard = true;  // 断言：保守放弃
                    i += 2;
                } else if (f == L':') {
                    i += 2;  // 非捕获组：照常收集
                } else if (f == L'<') {
                    size_t j = i + 3;
                    if (j < pattern.size() && (pattern[j] == L'=' ||
                                               pattern[j] == L'!')) {
                        stack.back().discard = true;  // lookbehind
                    }
                    while (j < pattern.size() && pattern[j] != L'>') ++j;
                    i = (j < pattern.size()) ? j : i + 2;
                } else {
                    stack.back().discard = true;  // 未知 (? 形态：保守
                    i += 1;
                }
            }
            continue;
        }
        if (c == L')') {
            flush();
            if (!stack.empty()) {
                GroupLevel g = std::move(stack.back());
                stack.pop_back();
                // 组后紧跟 ?/*/{0,n} → 组整体可选，丢弃其 runs
                bool optional = false;
                if (i + 1 < pattern.size()) {
                    wchar_t nx = pattern[i + 1];
                    if (nx == L'?' || nx == L'*') optional = true;
                    else if (nx == L'{') optional = brace_min_zero(i + 1);
                }
                if (!optional && !g.discard) {
                    for (auto& r : g.runs) dest().push_back(r);
                }
            }
            continue;
        }
        if (c == L'|') {
            flush();
            if (stack.empty()) {
                // 顶层 alternation：结束当前分支
                groups.push_back(std::move(cur_alt));
                cur_alt.clear();
            } else {
                stack.back().discard = true;  // 组内 alternation：整组放弃
            }
            continue;
        }
        if (c == L'?' || c == L'*') {
            // 量词使前一个字面字符可选 → 剔除后再断段
            if (!cur.empty()) cur.pop_back();
            flush();
            continue;
        }
        if (c == L'{') {
            // {m,n} 量词：内容非字面量，整体跳过；m==0 时前字面字符可选
            if (brace_min_zero(i) && !cur.empty()) cur.pop_back();
            flush();
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != L'}') ++j;
            i = j;  // 停在 '}'（循环 ++ 后越过）
            continue;
        }
        if (c == L'.' || c == L'+' || c == L'^' || c == L'$' ||
            c == L']' || c == L'}') {
            // 普通元字符 → 字面量段结束
            flush();
            continue;
        }
        cur.push_back(c);
    }
    flush();
    groups.push_back(std::move(cur_alt));
    return groups;
}

// ---- 序列化辅助 ----

std::string Indexer::serialize_u32_be(uint32_t v) const {
    std::string out(4, '\0');
    out[0] = static_cast<char>((v >> 24) & 0xFF);
    out[1] = static_cast<char>((v >> 16) & 0xFF);
    out[2] = static_cast<char>((v >> 8) & 0xFF);
    out[3] = static_cast<char>(v & 0xFF);
    return out;
}

std::vector<uint8_t> Indexer::serialize_u32_le(uint32_t v) const {
    std::vector<uint8_t> out(4);
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    return out;
}

std::vector<uint8_t> Indexer::serialize_i64_le(int64_t v) const {
    std::vector<uint8_t> out(8);
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((u >> (i * 8)) & 0xFF);
    }
    return out;
}

std::vector<uint8_t> Indexer::serialize_postings(
    const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& p) const {
    // varint + zigzag-delta：[count: varint] 然后 per tuple
    // [Δfid: zigvarint][Δline: zigvarint][Δcol: zigvarint]，baseline 0。
    // 调用方已按 (fid, line) 排序，相邻项差值小 → 多数 1 字节。
    std::vector<uint8_t> out;
    out.reserve(p.size() * 3 + 4);
    put_varint(out, static_cast<uint64_t>(p.size()));
    uint32_t prev_fid = 0, prev_line = 0, prev_col = 0;
    for (const auto& [fid, line, col] : p) {
        put_varint(out, zig_enc(static_cast<int32_t>(fid - prev_fid)));
        put_varint(out, zig_enc(static_cast<int32_t>(line - prev_line)));
        put_varint(out, zig_enc(static_cast<int32_t>(col - prev_col)));
        prev_fid = fid; prev_line = line; prev_col = col;
    }
    return out;
}

std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
Indexer::deserialize_postings(const std::vector<uint8_t>& data) const {
    return deserialize_postings(data.data(), data.size());
}

std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
Indexer::deserialize_postings(const uint8_t* data, size_t len) const {
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> result;
    size_t pos = 0;
    uint64_t count = 0;
    if (!get_varint_p(data, len, pos, count)) return result;
    // S5.5：reserve 前上限校验——每条 posting 至少 3 个 varint 字节，合法
    // count ≤ 剩余字节/3；损坏索引谎报巨大 count 时防止 reserve 巨量分配。
    // 截断数据本就在此 count 处提前返回，钳制不改变正常数据的行为。
    size_t max_possible = (pos <= len) ? ((len - pos) / 3) : 0;
    if (count > max_possible) count = max_possible;
    result.reserve(static_cast<size_t>(count));
    uint32_t prev_fid = 0, prev_line = 0, prev_col = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t df, dl, dc;
        if (!get_varint_p(data, len, pos, df) || !get_varint_p(data, len, pos, dl) ||
            !get_varint_p(data, len, pos, dc)) {
            return result;  // 截断/损坏 → 返回已解析部分（搜索视该 term 为部分命中）
        }
        uint32_t fid = prev_fid + static_cast<uint32_t>(zig_dec(static_cast<uint32_t>(df)));
        uint32_t line = prev_line + static_cast<uint32_t>(zig_dec(static_cast<uint32_t>(dl)));
        uint32_t col = prev_col + static_cast<uint32_t>(zig_dec(static_cast<uint32_t>(dc)));
        result.push_back({fid, line, col});
        prev_fid = fid; prev_line = line; prev_col = col;
    }
    return result;
}

std::vector<uint8_t> Indexer::serialize_file_ids(
    const std::set<uint32_t>& ids) const {
    // 升序 delta + varint：[count: varint][Δfid: varint]... baseline 0
    std::vector<uint8_t> out;
    out.reserve(ids.size() * 2 + 4);
    put_varint(out, static_cast<uint64_t>(ids.size()));
    uint32_t prev = 0;
    for (uint32_t fid : ids) {
        put_varint(out, static_cast<uint64_t>(fid - prev));
        prev = fid;
    }
    return out;
}

std::set<uint32_t> Indexer::deserialize_file_ids(
    const std::vector<uint8_t>& data) const {
    return deserialize_file_ids(data.data(), data.size());
}

std::set<uint32_t> Indexer::deserialize_file_ids(
    const uint8_t* data, size_t len) const {
    std::set<uint32_t> result;
    size_t pos = 0;
    uint64_t count = 0;
    if (!get_varint_p(data, len, pos, count)) return result;
    uint32_t prev = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t d;
        if (!get_varint_p(data, len, pos, d)) return result;
        uint32_t fid = prev + static_cast<uint32_t>(d);
        result.insert(fid);
        prev = fid;
    }
    return result;
}

std::vector<uint8_t> Indexer::serialize_file_record(
    const FileRecord& rec) const {
    // [path_len: uint16][path: UTF-8][mtime: int64 LE][size: int64 LE][enc: uint8]
    std::vector<uint8_t> out;
    uint16_t path_len = static_cast<uint16_t>(rec.path.size());
    out.resize(2 + rec.path.size() + 8 + 8 + 1);
    out[0] = static_cast<uint8_t>(path_len & 0xFF);
    out[1] = static_cast<uint8_t>((path_len >> 8) & 0xFF);
    size_t pos = 2;
    for (size_t i = 0; i < rec.path.size(); ++i) {
        out[pos++] = static_cast<uint8_t>(rec.path[i]);
    }
    uint64_t mt = static_cast<uint64_t>(rec.mtime);
    for (int i = 0; i < 8; ++i) {
        out[pos++] = static_cast<uint8_t>((mt >> (i * 8)) & 0xFF);
    }
    uint64_t sz = static_cast<uint64_t>(rec.size);
    for (int i = 0; i < 8; ++i) {
        out[pos++] = static_cast<uint8_t>((sz >> (i * 8)) & 0xFF);
    }
    out[pos] = rec.enc;
    return out;
}

bool Indexer::deserialize_file_record(const std::vector<uint8_t>& data,
                                      FileRecord& out) const {
    return deserialize_file_record(data.data(), data.size(), out);
}

bool Indexer::deserialize_file_record(const uint8_t* data, size_t len,
                                      FileRecord& out) const {
    if (len < 2 + 8 + 8 + 1) return false;
    uint16_t path_len = static_cast<uint16_t>(data[0]) |
                        (static_cast<uint16_t>(data[1]) << 8);
    if (len < static_cast<size_t>(2 + path_len + 8 + 8 + 1)) return false;
    out.path.assign(reinterpret_cast<const char*>(data + 2),
                    reinterpret_cast<const char*>(data + 2 + path_len));
    size_t pos = 2 + path_len;
    uint64_t mt = 0;
    for (int i = 0; i < 8; ++i) {
        mt |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
    }
    out.mtime = static_cast<int64_t>(mt);
    pos += 8;
    uint64_t sz = 0;
    for (int i = 0; i < 8; ++i) {
        sz |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
    }
    out.size = static_cast<int64_t>(sz);
    pos += 8;
    out.enc = data[pos];
    return true;
}

// ---- 回读文件指定行 ----

std::wstring Indexer::read_line(const std::filesystem::path& path,
                                int line_no) const {
    std::wstring text = read_text(path);
    if (text.empty()) return std::wstring();

    int current = 1;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n') {
            if (current == line_no) {
                std::wstring line = text.substr(start, i - start);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                return line;
            }
            ++current;
            start = i + 1;
        }
    }
    // 最后一行（无 \n 结束）
    if (current == line_no) {
        std::wstring line = text.substr(start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        return line;
    }
    return std::wstring();
}

// ---- 单文件索引（供 update_incremental 使用）----
// 对单个文件提取词项和 trigram，写入 IndexStore 的 postings/trigrams 子库。
// 返回 false = 非文本（is_textual 不过），此时不写任何 files/paths 条目，
// 调用方不得为其保留 file_id（新增文件）或须移除旧条目（变更文件变二进制）。
bool Indexer::index_single_file(const std::filesystem::path& path,
                                uint32_t file_id) {
    // 探测顺序：先 is_textual（二进制直接退出，不占索引），再算编码/大小/mtime
    std::vector<unsigned char> head = read_head_bytes(path, 8192);
    if (!is_textual(head)) return false;   // 二进制 → 不写任何条目
    Encoding enc = detect_encoding(head);

    std::error_code ec;
    auto fsize = std::filesystem::file_size(path, ec);
    int64_t size = ec ? 0 : static_cast<int64_t>(fsize);

    auto ftime = std::filesystem::last_write_time(path, ec);
    int64_t mtime = ec ? 0 : file_mtime_unix(ftime);

    // 存 files 子库：file_id(BE) → FileRecord
    FileRecord rec;
    rec.path = wstring_to_utf8(path.wstring());
    rec.mtime = mtime;
    rec.size = size;
    rec.enc = encoding_to_uint8(enc);
    store_.put("files", serialize_u32_be(file_id), serialize_file_record(rec));

    // 存 paths 子库：path(UTF-8) → file_id(BE)
    std::string fid_be = serialize_u32_be(file_id);
    store_.put("paths", rec.path,
               std::vector<uint8_t>(fid_be.begin(), fid_be.end()));

    // 🔴 修复：单文件超过阈值时走 MMF 流式索引，避免整文件读入导致 OOM。
    // B1（2026-10-26）：UTF-16 也流式化（for_each_line_mmap 已支持码元级切行），
    // 不再回退整文件读入的内存路径。
    const int64_t threshold_bytes =
        static_cast<int64_t>(bigfile_threshold_mb_) * 1024 * 1024;
    if (size > threshold_bytes) {
        index_single_file_large(path, file_id, enc);
        return true;
    }

    // 小文件：整文件读入内存（有界，安全）
    std::wstring text = read_text(path);

    if (text.empty()) return true;

    // 切行
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n') {
            std::wstring line = text.substr(start, i - start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(std::move(line));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        std::wstring line = text.substr(start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    // 文件级 trigram 去重集合
    std::set<std::string> file_trigrams;
    // v3：本文件全部 (term, fid, line, col) 先攒起来，按 term 分组后批量写回。
    //
    // 原写法是"每个 token 都把该 term 的整条 postings 读出来 → 追加 → 排序 →
    // 写回"。3-gram 补记后 postings 条目成倍增长，高频 3-gram（int/for/the）
    // 的 posting 链很长，逐 token 读改写会让增量更新退化成 O(n²)。
    // 批量后：同一 term 在本文件内只读一次、写一次。
    std::vector<std::pair<std::string,
                          std::tuple<uint32_t, uint32_t, uint32_t>>> batch;

    for (size_t li = 0; li < lines.size(); ++li) {
        const std::wstring& line = lines[li];
        uint32_t line_no = static_cast<uint32_t>(li + 1);

        // 词项切分
        auto tokens = tokenize_line(line);
        for (const auto& [term, col] : tokens) {
            std::string term_utf8 = wstring_to_utf8(to_lower_wstring(term));
            batch.emplace_back(std::move(term_utf8),
                               std::make_tuple(file_id, line_no,
                                               static_cast<uint32_t>(col)));
        }

        // trigram 提取（文件级去重）
        auto grams = extract_trigrams(line, true);  // 索引期统一 ignore_case
        for (const auto& g : grams) {
            file_trigrams.insert(g);
        }
    }

    // 按 term 归并：同一 term 只做一次「读 → 合并 → 排序 → 写回」
    std::sort(batch.begin(), batch.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < batch.size();) {
        size_t j = i;
        while (j < batch.size() && batch[j].first == batch[i].first) ++j;

        const std::string& term_utf8 = batch[i].first;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> plist;
        if (const std::vector<uint8_t>* pdata = store_.get("postings", term_utf8)) {
            plist = deserialize_postings(*pdata);
        }
        for (size_t k = i; k < j; ++k) plist.push_back(batch[k].second);
        std::sort(plist.begin(), plist.end());
        store_.put("postings", term_utf8, serialize_postings(plist));

        i = j;
    }

    // 将文件级去重 trigrams 写入映射
    for (const auto& g : file_trigrams) {
        std::set<uint32_t> fids;
        if (const std::vector<uint8_t>* pdata = store_.get("trigrams", g)) {
            fids = deserialize_file_ids(*pdata);
        }
        fids.insert(file_id);
        store_.put("trigrams", g, serialize_file_ids(fids));
    }
    return true;
}

// ---- 移除某文件的全部索引条目（postings / trigrams / files / paths）----
// 变更文件变成二进制时由 update_incremental 调用。O(postings 键数) 全扫，
// 属罕见路径，正确性优先。
void Indexer::remove_file_from_index(uint32_t fid,
                                     const std::filesystem::path& path) {
    std::string fid_be = serialize_u32_be(fid);

    for (const auto& key : store_.get_keys("postings")) {
        const std::vector<uint8_t>* pdata = store_.get("postings", key);
        if (!pdata) continue;
        auto plist = deserialize_postings(*pdata);
        bool touched = false;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> new_list;
        for (const auto& [f, line, col] : plist) {
            if (f == fid) { touched = true; continue; }
            new_list.push_back({f, line, col});
        }
        if (!touched) continue;
        if (new_list.empty()) store_.del("postings", key);
        else store_.put("postings", key, serialize_postings(new_list));
    }

    for (const auto& key : store_.get_keys("trigrams")) {
        const std::vector<uint8_t>* pdata = store_.get("trigrams", key);
        if (!pdata) continue;
        auto fids = deserialize_file_ids(*pdata);
        if (fids.find(fid) == fids.end()) continue;
        fids.erase(fid);
        if (fids.empty()) store_.del("trigrams", key);
        else store_.put("trigrams", key, serialize_file_ids(fids));
    }

    store_.del("files", fid_be);
    store_.del("paths", wstring_to_utf8(path.wstring()));
}

// ---- meta.ext_set 反序列化（原为两份重复实现，抽成单一 helper）----
std::set<std::wstring> Indexer::parse_meta_ext_set(const uint8_t* p,
                                                   size_t len) const {
    std::set<std::wstring> out;
    if (!p || len < 4) return out;
    uint32_t cnt = static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
    size_t pos = 4;
    for (uint32_t i = 0; i < cnt && pos + 2 <= len; ++i) {
        uint16_t l = static_cast<uint16_t>(p[pos]) |
                     (static_cast<uint16_t>(p[pos + 1]) << 8);
        pos += 2;
        if (pos + l > len) break;
        out.insert(utf8_to_wstring(std::string(p + pos, p + pos + l)));
        pos += l;
    }
    return out;
}

// ---- 建库时的扩展名集合（B3 静默错误提示用）----
std::set<std::wstring> Indexer::indexed_ext_set() const {
    size_t len = 0;
    const uint8_t* p = get_blob("meta", "ext_set", len);
    return parse_meta_ext_set(p, len);
}

// ---- flush：把累加器里各 term 的 postings 合并写回 store（读 → 追加 → 排序 → 写）----
void Indexer::flush_postings(
    std::unordered_map<std::string,
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>>& acc) {
    for (auto& kv : acc) {
        const std::string& term = kv.first;
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> plist;
        if (const std::vector<uint8_t>* pdata = store_.get("postings", term)) {
            plist = deserialize_postings(*pdata);
        }
        for (auto& t : kv.second) plist.push_back(std::move(t));
        std::sort(plist.begin(), plist.end());
        store_.put("postings", term, serialize_postings(plist));
    }
    acc.clear();
}

// ---- MMF 流式逐行读取（大文件索引与 build 大文件路径共用）----
// 与 Scanner::scan_big_file 同口径：滑动窗口映射（经 core/port，C1）、仅在
// 换行边界处切行、窗口边界不拆字符。每行回调 on_line。
// B1（2026-10-26）：UTF-16 同样流式化——'\n' 在 UTF-16 里是固定的 2 字节码元
// （LE: 0A 00 / BE: 00 0A），字节级定位「偶对齐的该码元」即可安全切分；
// 窗口起点 64MB 对齐、prev_tail 起于码元边界，码元对齐天然保持。
// 打开/映射失败返回 false（调用方决定跳过或回退策略，绝不整文件读入）。

// 窗口大小（默认 64MB；测试注入小窗口覆盖多窗口滑动路径）
static size_t g_index_mmap_window = 64ULL * 1024 * 1024;
void set_index_mmap_window(size_t n) {
    g_index_mmap_window = n > 0 ? n : 1;
}
size_t get_index_mmap_window() { return g_index_mmap_window; }

// 找 data[0..len) 内最后一个「完整换行符」的起始字节位置（npos=无）。
// abs_base = data[0] 在文件中的绝对偏移（UTF-16 码元对齐判定用）。
static size_t find_last_line_break(const char* data, size_t len, Encoding enc,
                                   uint64_t abs_base) {
    if (enc == Encoding::Utf16Le || enc == Encoding::Utf16Be) {
        if (len < 2) return std::string::npos;
        // 只在「绝对偏移为偶数」的码元起点检查（BOM 2 字节，全文件码元对齐）
        size_t start = len - 2;
        if (((abs_base + start) & 1ull) != 0) {
            if (start == 0) return std::string::npos;
            --start;
        }
        for (size_t k = start + 2; k >= 2; ) {
            k -= 2;
            unsigned char b0 = static_cast<unsigned char>(data[k]);
            unsigned char b1 = static_cast<unsigned char>(data[k + 1]);
            if (enc == Encoding::Utf16Le) {
                if (b0 == 0x0A && b1 == 0x00) return k;
            } else {
                if (b0 == 0x00 && b1 == 0x0A) return k;
            }
        }
        return std::string::npos;
    }
    return find_last_newline(data, len);
}

// UTF-16 区间解码：不经 decode_bytes——流式区间的 BOM 语义与整文件不同，
// 文件中部出现的 U+FEFF（零宽不换行空格）不应被当作 BOM 跳过，仅在文件
// 绝对起点为 0 的区间跳过真 BOM。
static std::wstring decode_utf16_region(const unsigned char* p, size_t n,
                                        bool big_endian, uint64_t abs_start) {
    size_t off = 0;
    if (abs_start == 0 && n >= 2) {
        wchar_t first = big_endian
            ? static_cast<wchar_t>((static_cast<unsigned char>(p[0]) << 8) |
                                    static_cast<unsigned char>(p[1]))
            : static_cast<wchar_t>((static_cast<unsigned char>(p[1]) << 8) |
                                    static_cast<unsigned char>(p[0]));
        if (first == 0xFEFF) off = 2;   // 文件首的真 BOM
    }
    std::wstring s;
    s.reserve((n - off) / 2);
    for (size_t i = off; i + 1 < n; i += 2) {
        s.push_back(big_endian
            ? static_cast<wchar_t>((static_cast<unsigned char>(p[i]) << 8) |
                                    static_cast<unsigned char>(p[i + 1]))
            : static_cast<wchar_t>((static_cast<unsigned char>(p[i + 1]) << 8) |
                                    static_cast<unsigned char>(p[i])));
    }
    return s;
}

// 换行完整区间的统一解码入口（UTF-8/GBK 走 decode_bytes；UTF-16 直解）
static std::wstring decode_line_region(const unsigned char* p, size_t n,
                                       Encoding enc, uint64_t abs_start) {
    if (enc == Encoding::Utf16Le || enc == Encoding::Utf16Be) {
        return decode_utf16_region(p, n, enc == Encoding::Utf16Be, abs_start);
    }
    std::vector<unsigned char> v(p, p + n);
    return decode_bytes(v, enc);
}

bool Indexer::for_each_line_mmap(
    const std::filesystem::path& path, Encoding enc,
    const std::function<void(int line_no, const std::wstring& line)>& on_line) const {
    port::SlidingFileMap sfm;
    if (!sfm.open(path)) {
        logger::warn(L"index: 大文件无法打开，跳过: " + path.wstring());
        return false;
    }
    const uint64_t file_size = sfm.size();
    if (file_size == 0) return false;

    const size_t WINDOW = g_index_mmap_window;
    const bool is_utf16 = (enc == Encoding::Utf16Le || enc == Encoding::Utf16Be);
    const size_t br_len = is_utf16 ? 2 : 1;   // 换行符字节长

    uint64_t offset = 0;
    std::string prev_tail;
    int line_no_offset = 0;
    bool ok = true;

    while (offset < file_size) {
        size_t map_size = static_cast<size_t>(
            std::min<uint64_t>(WINDOW, file_size - offset));
        const unsigned char* window = sfm.map(offset, map_size);
        if (window == nullptr) { ok = false; break; }

        std::string chunk;
        chunk.reserve(prev_tail.size() + map_size);
        chunk.append(prev_tail);
        chunk.append(reinterpret_cast<const char*>(window), map_size);
        // 下一次 sfm.map()/析构自动释放本窗口
        const uint64_t chunk_abs0 = offset - prev_tail.size();

        size_t last_br = find_last_line_break(chunk.data(), chunk.size(), enc,
                                              chunk_abs0);
        if (last_br == std::string::npos) {
            if (chunk.size() >= WINDOW) {
                // 整窗无换行 → 作为单行处理（极端超长行，罕见）
                std::wstring wtext = decode_line_region(
                    reinterpret_cast<const unsigned char*>(chunk.data()),
                    chunk.size(), enc, chunk_abs0);
                if (!wtext.empty()) {
                    if (wtext.back() == L'\r') wtext.pop_back();
                    on_line(line_no_offset + 1, wtext);
                    ++line_no_offset;
                }
                prev_tail.clear();
            } else {
                prev_tail = std::move(chunk);
            }
            offset += map_size;
            continue;
        }

        size_t decode_len = last_br + br_len;
        std::wstring wtext = decode_line_region(
            reinterpret_cast<const unsigned char*>(chunk.data()), decode_len,
            enc, chunk_abs0);
        size_t start = 0;
        int li = 0;
        for (size_t i = 0; i < wtext.size(); ++i) {
            if (wtext[i] == L'\n') {
                std::wstring line = wtext.substr(start, i - start);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                on_line(line_no_offset + li + 1, line);
                ++li;
                start = i + 1;
            }
        }
        line_no_offset += li;
        prev_tail = chunk.substr(decode_len);
        offset += map_size;
    }

    // 末尾剩余（文件未以换行结束的最后一行）
    if (ok && !prev_tail.empty()) {
        // prev_tail 起于最后一个完整换行之后；其绝对偏移 = 文件大小 - 长度
        std::wstring wtext = decode_line_region(
            reinterpret_cast<const unsigned char*>(prev_tail.data()),
            prev_tail.size(), enc, file_size - prev_tail.size());
        if (!wtext.empty()) {
            if (wtext.back() == L'\r') wtext.pop_back();
            on_line(line_no_offset + 1, wtext);
        }
    }

    return ok;
}

// ---- 大文件 MMF 流式索引 ----
// 逐行 tokenize + 提取 trigram，postings 攒到 kPostingFlushCap 即 flush 回
// store —— 内存有界（~数十 MB），彻底杜绝整文件读入的 OOM。
// MMF 不可用时记日志并跳过该文件：绝不回退 read_text，否则大文件仍会 OOM
// （这与 Scanner 的「MMF 失败回退 read_text」不同，索引侧 OOM 代价远高于漏一文件）。
void Indexer::index_single_file_large(const std::filesystem::path& path,
                                      uint32_t file_id, Encoding enc) {
    // postings 累加器（内存有界，到 kPostingFlushCap 即 flush）；文件级 trigram 去重集合
    std::unordered_map<std::string,
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>> acc;
    size_t pcount = 0;
    std::set<std::string> file_trigrams;

    for_each_line_mmap(path, enc,
        [this, &acc, &pcount, &file_trigrams, file_id](
            int line_no, const std::wstring& line) {
            uint32_t ln = static_cast<uint32_t>(line_no);
            auto tokens = tokenize_line(line);
            for (const auto& [term, col] : tokens) {
                std::string term_utf8 = wstring_to_utf8(to_lower_wstring(term));
                acc[term_utf8].emplace_back(file_id, ln,
                                            static_cast<uint32_t>(col));
                ++pcount;
                if (pcount >= kPostingFlushCap) {
                    flush_postings(acc);
                    pcount = 0;
                }
            }
            auto grams = extract_trigrams(line, true);
            for (const auto& g : grams) file_trigrams.insert(g);
        });

    // flush 余量
    if (!acc.empty()) flush_postings(acc);

    // 写文件级去重 trigrams
    for (const auto& g : file_trigrams) {
        std::set<uint32_t> fids;
        if (const std::vector<uint8_t>* pdata = store_.get("trigrams", g)) {
            fids = deserialize_file_ids(*pdata);
        }
        fids.insert(file_id);
        store_.put("trigrams", g, serialize_file_ids(fids));
    }
}

// ---- 获取索引中所有 file_id ----

std::set<uint32_t> Indexer::get_all_file_ids() const {
    std::set<uint32_t> fids;
    // mmap 模式下 store_ 为空 → 用 reader_ 取 keys；否则用 store_
    std::vector<std::string> keys = reader_ ? reader_->get_keys("files")
                                            : store_.get_keys("files");
    for (const auto& k : keys) {
        if (k.size() == 4) {
            uint32_t fid = (static_cast<uint32_t>(static_cast<uint8_t>(k[0])) << 24) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(k[1])) << 16) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(k[2])) << 8) |
                           static_cast<uint32_t>(static_cast<uint8_t>(k[3]));
            fids.insert(fid);
        }
    }
    return fids;
}

// ---- open ----

bool Indexer::open(const std::filesystem::path& root) {
    root_ = root;
    index_path_ = compute_index_path(root);

    // 优先 mmap 零拷贝读路径：open 只建轻量查找表（key 拷贝、value 不拷贝），
    // 避免 IndexStore::open 把整个索引读进内存 map 的几百万次堆分配。
    // 失败（文件不存在/magic/version/checksum 不符）→ 回退 IndexStore 传统 load。
    reader_ = std::make_unique<MmapIndexReader>();
    if (reader_->open(index_path_)) {
        available_ = true;
        return true;
    }
    reader_.reset();

    if (!store_.open(index_path_)) {
        available_ = false;
        return false;
    }
    available_ = true;
    return true;
}

// 统一读访问：mmap 优先，无则 store_ 兜底
const uint8_t* Indexer::get_blob(const std::string& db, const std::string& key,
                                 size_t& len) const {
    if (reader_) {
        return reader_->get(db, key, len);
    }
    const std::vector<uint8_t>* pv = store_.get(db, key);
    if (!pv) return nullptr;
    len = pv->size();
    return pv->data();
}

// ---- build（全量构建）----

bool Indexer::build(const std::filesystem::path& root,
                    const std::set<std::wstring>& ext_set,
                    const BuildProgress& on_progress) {
    root_ = root;
    index_path_ = compute_index_path(root);

    // 全量重建：关闭旧存储 + mmap reader，删除旧索引文件，重新创建
    store_.close();
    reader_.reset();   // 释放 mmap 句柄，否则删除索引文件会失败（占用中）
    invalidate_cache(root);   // 重建后 mtime 变，cache 会自然 miss；显式作废更稳
    global_query_cache().clear();   // A2 双保险：清查询缓存（键含 mtime/size 本可自然 miss）
    std::error_code ec;
    std::filesystem::remove_all(root / ".textsearch", ec);
    if (!store_.open(index_path_)) {
        available_ = false;
        return false;
    }

    // enumerate_files 获取文件列表
    auto files = enumerate_files(root, ext_set, true);

    // 过滤掉 .textsearch 目录下的文件（避免索引到自己的索引文件）
    std::vector<std::filesystem::path> filtered;
    for (const auto& f : files) {
        std::error_code rec_ec;
        std::wstring rel;
        try {
            rel = std::filesystem::relative(f, root, rec_ec).wstring();
        } catch (...) {
            rel.clear();
        }
        if (rel.find(L".textsearch") != std::wstring::npos) continue;
        filtered.push_back(f);
    }

    // 内存中积累 postings 和 trigrams（批量构建效率更高）
    // postings: term(UTF-8) → vector<(file_id, line, col)>
    std::map<std::string,
             std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>> postings_map;
    // trigrams: gram(UTF-8) → set<file_id>
    std::map<std::string, std::set<uint32_t>> trigrams_map;

    uint32_t nterms = 0;

    // ---- 并行建库：文件级并行 tokenize + trigram 提取 ----
    // 每个 worker 处理 filtered 的一个分片，产出 BuildFile（不触碰 store_，
    // 避免对 IndexStore 内部 map 的并发写）。file_id 用原子计数器分配；
    // 二进制文件也消费一个 id（产生空洞，但 file_id 仅作不透明主键，空洞无害，
    // get_all_file_ids / 增量更新都只遍历实际存储的 id 集合）。
    struct BuildFile {
        uint32_t file_id = 0;
        bool textual = false;
        FileRecord rec;
        std::string path_utf8;     // "paths" 子库 key
        // term_utf8 → 该文件的 (file_id, line, col)
        std::vector<std::pair<std::string,
                              std::tuple<uint32_t, uint32_t, uint32_t>>> postings;
        // 大文件流式路径专用：攒到 kPostingFlushCap 即归并进此 map —— 每条
        // occurrence 常驻内存只有 tuple 的 12B（term 串每词仅存一份），
        // 替代逐 occurrence 携带完整 term 副本的扁平 vector（~40-50B/条）。
        std::unordered_map<std::string,
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>> postings_by_term;
        std::vector<std::string> trigrams;  // 文件级去重 trigram
    };

    std::atomic<uint32_t> next_file_id{0};

    // 单文件处理（纯函数：只读 path + 调用 const 文本方法，不碰 store_）
    auto process_one = [this](const std::filesystem::path& path,
                              uint32_t fid) -> BuildFile {
        BuildFile bf;
        bf.file_id = fid;

        // 🔴 S2.1：大文件（> 阈值）走 MMF 流式，绝不整文件读入。
        // B1 起 UTF-16 同样流式（码元级切行），不再有 UTF-16 内存路径例外。
        // postings 攒到 kPostingFlushCap 即归并进 bf.postings_by_term（内存有界）。
        {
            std::error_code ec_sz;
            auto fsz = std::filesystem::file_size(path, ec_sz);
            int64_t file_sz = ec_sz ? 0 : static_cast<int64_t>(fsz);
            const int64_t threshold_bytes =
                static_cast<int64_t>(bigfile_threshold_mb_) * 1024 * 1024;
            if (file_sz > threshold_bytes) {
                std::vector<unsigned char> head = read_head_bytes(path, 8192);
                if (!is_textual(head)) return bf;   // 二进制 → textual=false
                Encoding enc = detect_encoding(head);
                {
                    bf.textual = true;
                    bf.rec.size = file_sz;
                    std::error_code ec_t;
                    auto ftime = std::filesystem::last_write_time(path, ec_t);
                    bf.rec.mtime = ec_t ? 0 : file_mtime_unix(ftime);
                    bf.rec.path = wstring_to_utf8(path.wstring());
                    bf.path_utf8 = bf.rec.path;
                    bf.rec.enc = encoding_to_uint8(enc);

                    std::vector<std::pair<std::string,
                          std::tuple<uint32_t, uint32_t, uint32_t>>> acc;
                    std::set<std::string> file_trigrams;
                    auto merge_acc = [&]() {
                        for (auto& [term, tup] : acc)
                            bf.postings_by_term[term].push_back(std::move(tup));
                        acc.clear();
                    };
                    bool ok = for_each_line_mmap(path, enc,
                        [&, fid](int line_no, const std::wstring& line) {
                            uint32_t ln = static_cast<uint32_t>(line_no);
                            auto tokens = tokenize_line(line);
                            for (const auto& [term, col] : tokens) {
                                acc.emplace_back(
                                    wstring_to_utf8(to_lower_wstring(term)),
                                    std::make_tuple(
                                        fid, ln, static_cast<uint32_t>(col)));
                                if (acc.size() >= kPostingFlushCap) merge_acc();
                            }
                            auto grams = extract_trigrams(line, true);
                            for (const auto& g : grams) file_trigrams.insert(g);
                        });
                    if (!ok) {
                        // MMF 失败：丢弃部分结果，按未索引处理（不写 rec，
                        // 增量更新会把它当新文件重试，避免永久静默漏文件）
                        BuildFile empty;
                        empty.file_id = fid;
                        return empty;
                    }
                    merge_acc();
                    bf.trigrams.assign(file_trigrams.begin(),
                                       file_trigrams.end());
                    return bf;
                }
            }
        }
        // 单次读全文：head 取前 8KB 做 is_textual/detect_encoding，省掉原先
        // read_head_bytes + read_text 两次 open+read（建库时每文件省一次系统调用）
        std::vector<unsigned char> bytes = read_file_bytes(path);
        if (bytes.empty()) {
            // 空文件：视为文本但无内容（不入 postings/trigrams，仍占一个 file_id）
            bf.textual = true;
            std::error_code ec;
            auto fsize = std::filesystem::file_size(path, ec);
            bf.rec.size = ec ? 0 : static_cast<int64_t>(fsize);
            auto ftime = std::filesystem::last_write_time(path, ec);
            bf.rec.mtime = ec ? 0 : file_mtime_unix(ftime);
            bf.rec.path = wstring_to_utf8(path.wstring());
            bf.path_utf8 = bf.rec.path;
            return bf;
        }
        size_t head_len = std::min<size_t>(bytes.size(), 8192);
        std::vector<unsigned char> head(bytes.begin(), bytes.begin() + head_len);
        if (!is_textual(head)) return bf;   // 二进制 → textual=false，空
        bf.textual = true;
        Encoding enc = detect_encoding(head);
        std::wstring text = decode_bytes(bytes, enc);

        std::error_code ec;
        bf.rec.size = static_cast<int64_t>(bytes.size());
        auto ftime = std::filesystem::last_write_time(path, ec);
        bf.rec.mtime = ec ? 0 : file_mtime_unix(ftime);
        bf.rec.path = wstring_to_utf8(path.wstring());
        bf.rec.enc = encoding_to_uint8(enc);
        bf.path_utf8 = bf.rec.path;

        if (text.empty()) return bf;
        // 切行
        std::vector<std::wstring> lines;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\n') {
                std::wstring line = text.substr(start, i - start);
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                lines.push_back(std::move(line));
                start = i + 1;
            }
        }
        if (start < text.size()) {
            std::wstring line = text.substr(start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(std::move(line));
        }
        std::set<std::string> file_trigrams;
        for (size_t li = 0; li < lines.size(); ++li) {
            const std::wstring& line = lines[li];
            uint32_t line_no = static_cast<uint32_t>(li + 1);
            auto tokens = tokenize_line(line);
            for (const auto& [term, col] : tokens) {
                std::string term_utf8 = wstring_to_utf8(to_lower_wstring(term));
                bf.postings.emplace_back(term_utf8,
                    std::make_tuple(fid, line_no, static_cast<uint32_t>(col)));
            }
            auto grams = extract_trigrams(line, true);
            for (const auto& g : grams) file_trigrams.insert(g);
        }
        bf.trigrams.assign(file_trigrams.begin(), file_trigrams.end());
        return bf;
    };

    size_t nfiles = filtered.size();
    // B3：建库进度（初始一次 (0,total)，之后每完成一个文件一次；
    // 并行 worker 会并发调用 on_progress，回调须线程安全）
    std::atomic<size_t> built_done{0};
    auto report_progress = [&](size_t d) {
        if (on_progress) on_progress(d, nfiles);
    };
    report_progress(0);

    size_t hw = std::thread::hardware_concurrency();
    size_t nw = hw ? hw : 4;
    if (nw < 2) nw = 2;
    if (nw > 16) nw = 16;
    if (nw > nfiles) nw = nfiles;
    if (nw < 1) nw = 1;

    std::vector<std::vector<BuildFile>> per_worker(nw);
    if (nw <= 1) {
        // 串行（极小目录，避免池开销）
        for (const auto& path : filtered) {
            uint32_t fid = next_file_id.fetch_add(1, std::memory_order_relaxed);
            per_worker[0].push_back(process_one(path, fid));
            report_progress(++built_done);
        }
    } else {
        ThreadPool pool(nw);
        size_t chunk = (nfiles + nw - 1) / nw;
        for (size_t w = 0; w < nw; ++w) {
            size_t begin = w * chunk;
            size_t end = std::min(begin + chunk, nfiles);
            if (begin >= end) continue;
            pool.enqueue([&, begin, end, w]() {
                auto& out = per_worker[w];
                out.reserve(end - begin);
                for (size_t i = begin; i < end; ++i) {
                    uint32_t fid = next_file_id.fetch_add(1, std::memory_order_relaxed);
                    out.push_back(process_one(filtered[i], fid));
                    report_progress(built_done.fetch_add(1,
                               std::memory_order_relaxed) + 1);
                }
            });
        }
        pool.wait_all();
    }

    // ---- 串行合并到 store_ + postings_map / trigrams_map ----
    uint32_t stored_count = 0;
    for (auto& vec : per_worker) {
        for (auto& bf : vec) {
            if (!bf.textual) continue;   // 二进制文件不入索引
            store_.put("files", serialize_u32_be(bf.file_id),
                       serialize_file_record(bf.rec));
            std::string fid_be = serialize_u32_be(bf.file_id);
            store_.put("paths", bf.path_utf8,
                       std::vector<uint8_t>(fid_be.begin(), fid_be.end()));
            for (auto& [term, tup] : bf.postings) {
                postings_map[term].push_back(tup);
                ++nterms;
            }
            for (auto& [term, plist] : bf.postings_by_term) {
                auto& dst = postings_map[term];
                dst.insert(dst.end(), std::make_move_iterator(plist.begin()),
                           std::make_move_iterator(plist.end()));
                nterms += static_cast<uint32_t>(plist.size());
            }
            for (auto& g : bf.trigrams) {
                trigrams_map[g].insert(bf.file_id);
            }
            ++stored_count;
        }
    }
    uint32_t file_id = stored_count;   // 兼容下方 meta "nfiles"

    // 序列化 postings 到 store
    for (const auto& [term, plist] : postings_map) {
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> sorted = plist;
        std::sort(sorted.begin(), sorted.end());
        store_.put("postings", term, serialize_postings(sorted));
    }

    // 序列化 trigrams 到 store
    for (const auto& [gram, fids] : trigrams_map) {
        store_.put("trigrams", gram, serialize_file_ids(fids));
    }

    // 存 meta 子库
    store_.put("meta", "version", serialize_u32_le(kIndexContentVersion));
    {
        std::string root_utf8 = wstring_to_utf8(root.wstring());
        store_.put("meta", "root",
                   std::vector<uint8_t>(root_utf8.begin(), root_utf8.end()));
    }
    store_.put("meta", "built_unix", serialize_i64_le(
        static_cast<int64_t>(std::time(nullptr))));
    store_.put("meta", "nfiles", serialize_u32_le(file_id));
    store_.put("meta", "nterms", serialize_u32_le(nterms));
    // 存储 ext_set（供 update_incremental 恢复）
    // 格式：[count: uint32][ext_len: uint16][ext: UTF-8]...
    {
        std::vector<uint8_t> ext_data;
        uint32_t ext_count = static_cast<uint32_t>(ext_set.size());
        ext_data.resize(4);
        ext_data[0] = static_cast<uint8_t>(ext_count & 0xFF);
        ext_data[1] = static_cast<uint8_t>((ext_count >> 8) & 0xFF);
        ext_data[2] = static_cast<uint8_t>((ext_count >> 16) & 0xFF);
        ext_data[3] = static_cast<uint8_t>((ext_count >> 24) & 0xFF);
        for (const auto& ext : ext_set) {
            std::string ext_utf8 = wstring_to_utf8(ext);
            uint16_t ext_len = static_cast<uint16_t>(ext_utf8.size());
            ext_data.push_back(static_cast<uint8_t>(ext_len & 0xFF));
            ext_data.push_back(static_cast<uint8_t>((ext_len >> 8) & 0xFF));
            ext_data.insert(ext_data.end(), ext_utf8.begin(), ext_utf8.end());
        }
        store_.put("meta", "ext_set", ext_data);
    }
    // checksum 由 IndexStore 文件头自动管理（FNV-1a）

    // 提交
    if (!store_.commit()) {
        available_ = false;
        return false;
    }

    available_ = true;
    return true;
}

// ---- search ----

std::vector<MatchResult> Indexer::search(const std::wstring& pattern,
                                         const SearchOptions& opts,
                                         const std::set<std::wstring>& ext_set) {
    std::vector<MatchResult> results;
    if (!available_ || pattern.empty()) return results;

    // ---- A2：查询结果 LRU 缓存 ----
    // 键在入口现取：索引文件 mtime+size 此刻必然反映「update_incremental 之后」
    // 的状态（scanner 的索引路由先增量更新再 search；增量更新改写索引文件，
    // mtime/size 变化 → 键必然 miss）。content_version 参与比较，索引内容
    // 口径升级（kIndexContentVersion +1）后旧条目全部失效。
    QueryCache::Key key;
    key.pattern = pattern;
    key.use_regex = opts.use_regex;
    key.ignore_case = opts.ignore_case;
    key.whole_word = opts.whole_word;
    key.recursive = opts.recursive;
    key.root = root_.wstring();
    for (const auto& e : ext_set) key.ext_set_joined += (e + L'\x1F');
    key.content_version = kIndexContentVersion;
    {
        std::error_code ec;
        auto mt = std::filesystem::last_write_time(index_path_, ec);
        key.index_mtime = ec ? 0 : file_mtime_unix(mt);
        auto sz = std::filesystem::file_size(index_path_, ec);
        key.index_size = ec ? -1 : static_cast<int64_t>(sz);
    }
    if (global_query_cache().get(key, results)) {
        return results;   // 命中：直接返回上次同参查询结果的拷贝
    }

    // 索引模式需手动应用用户当前勾选的扩展名集合（实时模式由 enumerate_files
    // 过滤文件列表，索引模式是全索引搜索，扩展名过滤必须在此补上，否则
    // 用户取消勾选的扩展名（如 .txt）仍会出现在结果中）。
    auto apply_ext_filter = [&](std::vector<MatchResult> r)
        -> std::vector<MatchResult> {
        if (ext_set.empty()) return r;  // 空集 → 不过滤
        std::vector<MatchResult> out;
        out.reserve(r.size());
        for (auto& m : r) {
            std::wstring ext = m.file_path.has_extension()
                ? to_lower_wstring(m.file_path.extension().wstring())
                : std::wstring();  // 无扩展名文件
            if (ext_set.count(ext) > 0) {
                out.push_back(std::move(m));
            }
        }
        return out;
    };

    std::vector<MatchResult> r = opts.use_regex
        ? search_regex(pattern, opts)
        : search_substring(pattern, opts);
    r = apply_ext_filter(std::move(r));

    // 写缓存守卫（TODO A2 的两项「不缓存」）：
    // ① 达 max_results 上限 → 结果被截断，内容依赖遍历时序，不可复用；
    // ② B3 告警路径 → 用户勾选了索引内不存在的扩展名，结果静默不完整
    //    （判定口径与 scanner.cpp 的 warn_unindexed_extensions 一致：双方
    //    小写后比对，任一集合为空视为无法判定）。该状态下不缓存；用户按
    //    B3 提示重建索引后 mtime 变化，新键自然生效。
    bool capped = r.size() >= opts.max_results;
    bool b3_path = false;
    if (!ext_set.empty()) {
        std::set<std::wstring> built = indexed_ext_set();
        if (!built.empty()) {
            for (const auto& e : ext_set) {
                if (built.count(to_lower_wstring(e)) == 0) {
                    b3_path = true;
                    break;
                }
            }
        }
    }
    if (!capped && !b3_path) {
        global_query_cache().put(key, r);
    }
    return r;
}

// ---- 子串搜索（bi-gram + 邻接验证 F.2.2）----

std::vector<MatchResult> Indexer::search_substring(
    const std::wstring& pattern, const SearchOptions& opts) {
    std::vector<MatchResult> results;

    // 切分 pattern
    auto tokens = tokenize_pattern(pattern);

    if (tokens.empty()) {
        // pattern 太短或全是分隔符 → 回退全文件搜索
        return search_fallback_all(pattern, opts);
    }

    // 构建 MatchFlags
    int flags = MatchFlags::Substring;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    // 修复 P1：原本 read_line() 每次命中都把整个文件重读一遍并按行从头扫描，
    // 放大可达数千倍（实测 103MB 单文件 + 高频词 2M 命中 → 直接卡死）。
    // 改为：把需要精筛的 (fid, 行号) 收集好，每个文件只打开/扫描一次，
    // 在单趟扫描中命中即精筛（零漏报）。LRU 容量限制同时打开的文件数，控制内存。
    struct CachedFile { std::filesystem::path path; std::wstring text; };
    std::list<std::pair<uint32_t, CachedFile>> file_lru_;
    static const size_t kFileCacheCap = 16;
    auto get_file = [&](uint32_t fid) -> const CachedFile* {
        for (auto it = file_lru_.begin(); it != file_lru_.end(); ++it) {
            if (it->first == fid) {
                if (it != file_lru_.begin()) {
                    auto val = std::move(*it);
                    file_lru_.erase(it);
                    file_lru_.push_front(std::move(val));
                }
                return &file_lru_.front().second;
            }
        }
        size_t flen;
        const uint8_t* pfdata = get_blob("files", serialize_u32_be(fid), flen);
        if (!pfdata) return nullptr;
        FileRecord rec;
        if (!deserialize_file_record(pfdata, flen, rec)) return nullptr;
        CachedFile cf;
        cf.path = utf8_to_wstring(rec.path);
        cf.text = read_text(cf.path);
        if (file_lru_.size() >= kFileCacheCap) file_lru_.pop_back();
        file_lru_.push_front({fid, std::move(cf)});
        return &file_lru_.front().second;
    };
    // 单文件单趟扫描：text 已读入缓存，逐行检查行号是否在 want_lines，命中则精筛。
    // 复杂度 O(文件字节数)，与命中行数无关 —— 这是消除放大效应的核心。
    auto process_file = [&](uint32_t fid, const std::set<uint32_t>& want_lines) {
        if (want_lines.empty() || results.size() >= opts.max_results) return;
        const CachedFile* cf = get_file(fid);
        if (!cf) return;
        const std::wstring& text = cf->text;
        if (text.empty()) return;
        int current = 1;
        size_t start = 0;
        auto emit = [&](size_t end_excl) {
            if (results.size() >= opts.max_results) return;
            if (!want_lines.count(current)) return;
            std::wstring line = text.substr(start, end_excl - start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) return;
            std::wstring single_line = line + L"\n";
            auto hits = search_in_text(single_line, pattern, flags, cf->path,
                                       opts.max_results - results.size());
            for (auto& h : hits) {
                h.line_no = current;
                h.line_text = line;
                results.push_back(std::move(h));
                if (results.size() >= opts.max_results) return;
            }
        };
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\n') {
                emit(i);
                ++current;
                start = i + 1;
                if (results.size() >= opts.max_results) return;
            }
        }
        emit(text.size());
    };

    // v3 子串召回：把 ASCII 词项展开成 3-gram 序列
    //
    // 为什么必须展开：postings 里的整词 term 只覆盖"整词出现"，查询词若是
    // 某个更长 token 的内部子串（rdp ⊂ wizardp、mmap ⊂ MmapIndexReader），
    // 整词 posting 里根本没有它 → 整行漏报。索引期已为每个长度 >= 4 的
    // ASCII token 补记了全部 3-gram 及列号，这里把查询词同样展开成 3-gram，
    // 复用下面的邻接验证即可零漏报（索引期/查询期口径一致是关键）。
    //
    // CJK 词项不展开：tokenize_pattern 已把 CJK 切成 bi-gram，索引期对 CJK
    // 同样是逐 bi-gram 记 posting，口径本来一致，无需展开。
    std::vector<PatternToken> expanded;
    expanded.reserve(tokens.size() * 2);
    bool need_full_scan = false;
    for (const auto& tok : tokens) {
        if (is_cjk(tok.term[0])) {
            // CJK bi-gram：与索引期口径一致，直接查
            expanded.push_back(tok);
        } else if (tok.term.size() >= 3) {
            // ASCII 且长度 >= 3 → 展开为 3-gram，保持各自在 pattern 中的偏移
            for (size_t k = 0; k + 3 <= tok.term.size(); ++k) {
                expanded.push_back({tok.term.substr(k, 3),
                                    tok.offset + static_cast<int>(k), 3});
            }
        } else {
            // ASCII 且长度 < 3：3-gram 索引覆盖不到（索引期同样只到 3-gram），
            // postings 不完备 → 整库扫描，保证零漏报（代价与实时模式相同）。
            need_full_scan = true;
            break;
        }
    }
    if (need_full_scan || expanded.empty()) {
        return search_fallback_all(pattern, opts);
    }

    // 单 term（CJK bi-gram，或长度恰为 3 的 ASCII 词）→ 直接查 postings
    if (expanded.size() == 1) {
        std::string term_utf8 = wstring_to_utf8(to_lower_wstring(expanded[0].term));
        size_t plen;
        const uint8_t* pdata = get_blob("postings", term_utf8, plen);
        if (!pdata) return results;
        auto postings = deserialize_postings(pdata, plen);

        // 同一行的每个匹配列位置都会产生一条 posting，而 search_in_text
        // 一次就会返回该行的全部匹配。若不去重，结果数会按 Σ(c²) 放大
        // （c = 该行匹配次数）—— 2026-08-31 会话检索实验发现的 P0：
        // 「会话」真实 885 条，索引却报 17305 条；内存按 c²×行长膨胀，
        // 会话语料单行最长 115KB，实测直接 bad_alloc 崩溃。
        // 这里按 (fid, line) 去重，每个文件只扫描一次（P1 修复）。
        std::map<uint32_t, std::set<uint32_t>> need_lines;
        for (const auto& [fid, line, col] : postings) {
            need_lines[fid].insert(line);
        }
        for (const auto& [fid, lines] : need_lines) {
            process_file(fid, lines);
            if (results.size() >= opts.max_results) break;
        }
        return results;
    }

    // 多 term → 各查 postings，先按 (fid, line) 求交，再做邻接验证（F.2.2）
    //
    // 3-gram 展开后 postings 可能很长（如 "mma" / "map"）。旧写法先把所有
    // term 的 (fid,line,col) 摊进一张大表再筛，会为"只命中部分 term"的残缺
    // 项分配内存，3-gram 规模下足以撑爆。改为：以 postings 最短的 term 作
    // 驱动，只保留同时出现在全部 term 表中的 (fid, line)，再在其上验证。
    std::vector<std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>>> maps(
        expanded.size());
    size_t drv = 0;
    for (size_t ti = 0; ti < expanded.size(); ++ti) {
        std::string term_utf8 = wstring_to_utf8(to_lower_wstring(expanded[ti].term));
        size_t plen;
        const uint8_t* pdata = get_blob("postings", term_utf8, plen);
        if (!pdata) return results;  // 任一 term 缺失 → 该 pattern 无匹配
        auto postings = deserialize_postings(pdata, plen);
        for (const auto& [fid, line, col] : postings) {
            maps[ti][std::make_pair(fid, line)].insert(col);
        }
        if (maps[ti].size() < maps[drv].size()) drv = ti;
    }

    std::set<std::pair<uint32_t, uint32_t>> verified;
    for (const auto& [key, drv_cols] : maps[drv]) {
        // 该 (fid, line) 必须同时出现在其余所有 term 的 posting 表中
        bool present = true;
        for (size_t ti = 0; ti < expanded.size() && present; ++ti) {
            if (ti != drv && maps[ti].find(key) == maps[ti].end()) present = false;
        }
        if (!present) continue;

        // 邻接验证：驱动 term 命中在列 c，则其余 term i 应命中在
        //     c + (offset_i - offset_drv)
        // 旧代码写成 c + offset_i，隐含假设"第一个 term 的 offset 为 0"；
        // pattern 以分隔符开头时该假设不成立（既有偏差，此处一并修正）。
        for (uint32_t c : drv_cols) {
            bool ok = true;
            for (size_t ti = 0; ti < expanded.size() && ok; ++ti) {
                if (ti == drv) continue;
                int64_t expected =
                    static_cast<int64_t>(c) +
                    (static_cast<int64_t>(expanded[ti].offset) -
                     static_cast<int64_t>(expanded[drv].offset));
                if (expected < 0) { ok = false; break; }
                const auto& s = maps[ti].find(key)->second;
                if (s.find(static_cast<uint32_t>(expected)) == s.end()) ok = false;
            }
            if (ok) {
                verified.insert(key);
                break;  // 同一 (file_id, line) 只需一个验证通过即可
            }
        }
    }

    // 对验证通过的 (file_id, line) 按文件分组，单趟扫描精筛（P1 修复）
    //
    // 精筛用原始 pattern + search_in_text 复核，故上面的邻接验证只需保证
    // "不漏"（可以有假阳性），最终列号与命中由 search_in_text 精确给出 ——
    // postings 仍带 col，高亮不需要二次扫描。
    std::map<uint32_t, std::set<uint32_t>> need_lines;
    for (const auto& [fid, line] : verified) {
        need_lines[fid].insert(line);
    }
    for (const auto& [fid, lines] : need_lines) {
        process_file(fid, lines);
        if (results.size() >= opts.max_results) break;
    }

    return results;
}

// ---- 正则搜索（trigram 加速 E.3）----

std::vector<MatchResult> Indexer::search_regex(
    const std::wstring& pattern, const SearchOptions& opts) {
    std::vector<MatchResult> results;

    // 提取字面量段（长度≥3），按顶层 '|' 分支分组
    auto groups = extract_literal_runs(pattern);

    // 构建 MatchFlags
    int flags = MatchFlags::Regex;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    std::set<uint32_t> candidate_files;
    bool fallback_all = false;  // 任一分支无可筛字面段 → 全文件

    // 候选集语义：组内 run 交集，分支间并集。
    // 索引期 trigram 统一小写（extract_trigrams(line, true)），查询期
    // 必须同样无条件 to_lower —— trigram 只做候选筛选，大小写精筛由
    // search_in_text 的 flags 完成（S1.1：大小写敏感 pattern 曾漏报）。
    for (const auto& grp : groups) {
        std::set<uint32_t> grp_files;
        bool grp_has_run = false;  // 该分支是否已有可用 run
        bool grp_dead = false;     // 某必选 run 候选为空 → 分支无候选

        for (const auto& run : grp) {
            std::set<std::string> grams;
            std::wstring s = to_lower_wstring(run);
            for (size_t i = 0; i + 3 <= s.size(); ++i) {
                grams.insert(wstring_to_utf8(s.substr(i, 3)));
            }
            if (grams.empty()) continue;

            // 各 gram 的 file_id 集合取交集 → 该 run 的候选文件
            std::set<uint32_t> run_files;
            bool first_gram = true;
            for (const auto& g : grams) {
                std::set<uint32_t> fid_set;
                size_t tlen;
                if (const uint8_t* ptdata = get_blob("trigrams", g, tlen)) {
                    fid_set = deserialize_file_ids(ptdata, tlen);
                }
                if (first_gram) {
                    run_files = std::move(fid_set);
                    first_gram = false;
                } else {
                    std::set<uint32_t> inter;
                    std::set_intersection(run_files.begin(), run_files.end(),
                                          fid_set.begin(), fid_set.end(),
                                          std::inserter(inter, inter.end()));
                    run_files = std::move(inter);
                }
                if (run_files.empty()) break;
            }

            if (!grp_has_run) {
                grp_files = std::move(run_files);
                grp_has_run = true;
            } else {
                std::set<uint32_t> inter;
                std::set_intersection(grp_files.begin(), grp_files.end(),
                                      run_files.begin(), run_files.end(),
                                      std::inserter(inter, inter.end()));
                grp_files = std::move(inter);
            }
            if (grp_files.empty()) {
                grp_dead = true;
                break;
            }
        }

        if (!grp_has_run) {
            // 该分支无长度≥3 必选字面段 → 无法筛选，整体回退全文件
            fallback_all = true;
            break;
        }
        if (grp_dead) continue;  // 分支必选段无候选：并集不受影响
        // 分支间并集
        candidate_files.insert(grp_files.begin(), grp_files.end());
    }

    if (fallback_all) {
        candidate_files = get_all_file_ids();
    }

    // 对候选文件逐个 search_in_text 精筛（零漏报）
    for (uint32_t fid : candidate_files) {
        if (results.size() >= opts.max_results) break;
        size_t flen;
        const uint8_t* pfdata = get_blob("files", serialize_u32_be(fid), flen);
        if (!pfdata) continue;
        FileRecord rec;
        if (!deserialize_file_record(pfdata, flen, rec)) continue;
        std::filesystem::path path = utf8_to_wstring(rec.path);

        std::wstring text = read_text(path);
        if (text.empty()) continue;

        // S4：单文件正则超时透传（matcher 内逐行检查，超时返回已得结果）
        double timeout = opts.search_timeout_sec > 0
                             ? static_cast<double>(opts.search_timeout_sec) : 0.0;
        auto hits = search_in_text(text, pattern, flags, path,
                                    opts.max_results - results.size(), timeout);
        for (auto& h : hits) {
            results.push_back(std::move(h));
            if (results.size() >= opts.max_results) break;
        }
    }

    return results;
}

// ---- 回退全文件搜索（pattern 无法 tokenize 时）----

std::vector<MatchResult> Indexer::search_fallback_all(
    const std::wstring& pattern, const SearchOptions& opts) {
    std::vector<MatchResult> results;

    int flags = MatchFlags::Substring;
    if (opts.ignore_case) flags |= MatchFlags::IgnoreCase;
    if (opts.whole_word) flags |= MatchFlags::WholeWord;

    auto fids = get_all_file_ids();
    for (uint32_t fid : fids) {
        if (results.size() >= opts.max_results) break;
        size_t flen;
        const uint8_t* pfdata = get_blob("files", serialize_u32_be(fid), flen);
        if (!pfdata) continue;
        FileRecord rec;
        if (!deserialize_file_record(pfdata, flen, rec)) continue;
        std::filesystem::path path = utf8_to_wstring(rec.path);

        std::wstring text = read_text(path);
        if (text.empty()) continue;

        auto hits = search_in_text(text, pattern, flags, path,
                                    opts.max_results - results.size());
        for (auto& h : hits) {
            results.push_back(std::move(h));
            if (results.size() >= opts.max_results) break;
        }
    }
    return results;
}

// ---- 增量更新（mtime/size 对比）----

bool Indexer::update_incremental(size_t* changed_count) {
    if (!available_) return false;
    if (changed_count) *changed_count = 0;

    // ---- 快速预检：无变化时避免 store_.open 的全量 load ----
    // 索引可能是 mmap 打开（零分配读路径），而写回必须切到 IndexStore：
    // store_.open() 会把整个索引读进内存 map（数百万次堆分配），commit() 还要全量写回，
    // 代价很高。因此先用 mmap 只读取出「已索引文件清单 + 各自 mtime/size」与当前目录对比；
    // 完全一致则无需写回，直接返回（省掉 load + save 两次大开销）。
    {
        size_t rlen = 0;
        const uint8_t* rp = get_blob("meta", "root", rlen);
        if (!rp) return false;
        std::wstring pre_root = utf8_to_wstring(std::string(rp, rp + rlen));

        // 从 meta.ext_set 恢复扩展名集合（格式：[count:u32][len:u16][bytes]...）
        // 注意：必须先把 get_blob 的结果单独落到一个变量，不能写成
        // parse_meta_ext_set(get_blob(..., elen), elen) —— C++ 未指定实参求值顺序，
        // elen 可能在 get_blob 写入前被读取（曾致 4 个增量测试集体失败）。
        size_t elen = 0;
        const uint8_t* ep = get_blob("meta", "ext_set", elen);
        std::set<std::wstring> pre_ext = parse_meta_ext_set(ep, elen);
        if (pre_ext.empty()) return false;   // 无 ext_set 无法可靠对比 → 保守返回失败

        auto cur_files = enumerate_files(pre_root, pre_ext, true);

        // 已索引文件清单：paths 子库（path → fid）+ files 子库（fid → FileRecord）
        std::vector<std::string> path_keys = reader_ ? reader_->get_keys("paths")
                                                     : store_.get_keys("paths");
        // 语义：目录中每个文件要么①已在索引且 mtime/size 一致，要么②本就
        // 不该被索引（is_textual 不过的二进制）；且索引里没有多余条目
        // （matched == path_keys.size()，覆盖"已索引文件被删除"的情形）。
        // 满足 → 索引已是最新，直接返回（不触发全量 load/save）。
        bool all_same = true;
        size_t matched = 0;
        for (const auto& p : cur_files) {
            // 与慢路径同口径：跳过 .textsearch 自身
            std::error_code rec_ec;
            std::wstring rel = std::filesystem::relative(p, pre_root, rec_ec).wstring();
            if (rel.find(L".textsearch") != std::wstring::npos) continue;

            std::string key = wstring_to_utf8(p.wstring());
            size_t plen = 0;
            const uint8_t* pv = get_blob("paths", key, plen);
            if (!pv || plen != 4) {
                // 不在索引里：二进制文件属"预期缺席"；否则是新增文件
                if (!is_textual(read_head_bytes(p, 8192))) continue;
                all_same = false;
                break;
            }
            uint32_t fid = (static_cast<uint32_t>(pv[0]) << 24) |
                           (static_cast<uint32_t>(pv[1]) << 16) |
                           (static_cast<uint32_t>(pv[2]) << 8)  |
                            static_cast<uint32_t>(pv[3]);
            size_t flen = 0;
            const uint8_t* fr = get_blob("files", serialize_u32_be(fid), flen);
            FileRecord rec;
            if (!fr || !deserialize_file_record(fr, flen, rec)) { all_same = false; break; }

            std::error_code ec;
            auto fsz = std::filesystem::file_size(p, ec);
            int64_t cur_size = ec ? 0 : static_cast<int64_t>(fsz);
            auto ft = std::filesystem::last_write_time(p, ec);
            int64_t cur_mtime = ec ? 0 : file_mtime_unix(ft);
            if (cur_mtime != rec.mtime || cur_size != rec.size) { all_same = false; break; }
            ++matched;
        }
        if (all_same && matched == path_keys.size()) {
            return true;   // 索引已是最新，无需写回
        }
    }

    // 增量更新要写回 store_，而 mmap reader_ 是只读视图 → 必须切到可读写模式。
    // 关闭 reader_，用 store_ 重新 load（带 verify）。后续 put/del/commit 走 store_。
    if (reader_) {
        reader_.reset();
        if (!store_.open(index_path_)) {
            available_ = false;
            return false;
        }
    }

    // 读取 meta 中的 root
    std::vector<uint8_t> root_data;
    if (!store_.get("meta", "root", root_data)) return false;
    std::string root_utf8(root_data.begin(), root_data.end());
    root_ = utf8_to_wstring(root_utf8);
    index_path_ = compute_index_path(root_);

    // 读取 ext_set（从 meta 中恢复）
    std::vector<uint8_t> ext_data;
    std::set<std::wstring> ext_set =
        store_.get("meta", "ext_set", ext_data)
            ? parse_meta_ext_set(ext_data.data(), ext_data.size())
            : std::set<std::wstring>();
    if (ext_set.empty()) return false;

    // 获取当前文件列表
    auto current_files = enumerate_files(root_, ext_set, true);

    // 过滤 .textsearch
    std::set<std::filesystem::path> current_set;
    for (const auto& f : current_files) {
        std::error_code rec_ec;
        std::wstring rel = std::filesystem::relative(f, root_, rec_ec).wstring();
        if (rel.find(L".textsearch") != std::wstring::npos) continue;
        current_set.insert(f);
    }

    // 获取索引中的文件列表（从 paths 子库）
    auto paths_keys = store_.get_keys("paths");
    std::map<std::filesystem::path, uint32_t> indexed_paths;
    for (const auto& key : paths_keys) {
        std::filesystem::path p = utf8_to_wstring(key);
        std::vector<uint8_t> fid_data;
        if (store_.get("paths", key, fid_data) && fid_data.size() == 4) {
            uint32_t fid =
                (static_cast<uint32_t>(fid_data[0]) << 24) |
                (static_cast<uint32_t>(fid_data[1]) << 16) |
                (static_cast<uint32_t>(fid_data[2]) << 8) |
                static_cast<uint32_t>(fid_data[3]);
            indexed_paths[p] = fid;
        }
    }

    // 找出新增、变更、删除的文件
    uint32_t max_fid = 0;
    for (const auto& [p, fid] : indexed_paths) {
        if (fid > max_fid) max_fid = fid;
    }

    std::vector<std::pair<std::filesystem::path, uint32_t>> deleted_files;
    std::vector<std::pair<std::filesystem::path, uint32_t>> changed_files;
    std::vector<std::filesystem::path> new_files;

    // 检测删除和变更
    for (const auto& [p, fid] : indexed_paths) {
        if (current_set.find(p) == current_set.end()) {
            deleted_files.push_back({p, fid});
            continue;
        }
        // 检查 mtime/size
        if (const std::vector<uint8_t>* pfdata = store_.get("files", serialize_u32_be(fid))) {
            FileRecord rec;
            if (deserialize_file_record(*pfdata, rec)) {
                std::error_code ec;
                auto fsize = std::filesystem::file_size(p, ec);
                int64_t cur_size = ec ? 0 : static_cast<int64_t>(fsize);
                auto ftime = std::filesystem::last_write_time(p, ec);
                int64_t cur_mtime = ec ? 0 : file_mtime_unix(ftime);
                if (cur_mtime != rec.mtime || cur_size != rec.size) {
                    changed_files.push_back({p, fid});
                }
            }
        }
    }

    // 检测新增
    for (const auto& p : current_set) {
        if (indexed_paths.find(p) == indexed_paths.end()) {
            new_files.push_back(p);
        }
    }

    // 处理删除和变更：从 postings 和 trigrams 中移除对应 file_id
    std::set<uint32_t> fids_to_remove;
    for (const auto& [p, fid] : deleted_files) fids_to_remove.insert(fid);
    for (const auto& [p, fid] : changed_files) fids_to_remove.insert(fid);

    if (!fids_to_remove.empty()) {
        // 从 postings 移除
        auto posting_keys = store_.get_keys("postings");
        for (const auto& key : posting_keys) {
            const std::vector<uint8_t>* pdata = store_.get("postings", key);
            if (!pdata) continue;
            auto plist = deserialize_postings(*pdata);
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> new_list;
            for (const auto& [fid, line, col] : plist) {
                if (fids_to_remove.find(fid) == fids_to_remove.end()) {
                    new_list.push_back({fid, line, col});
                }
            }
            if (new_list.empty()) {
                store_.del("postings", key);
            } else {
                store_.put("postings", key, serialize_postings(new_list));
            }
        }

        // 从 trigrams 移除
        auto tri_keys = store_.get_keys("trigrams");
        for (const auto& key : tri_keys) {
            const std::vector<uint8_t>* pdata = store_.get("trigrams", key);
            if (!pdata) continue;
            auto fids = deserialize_file_ids(*pdata);
            std::set<uint32_t> new_fids;
            for (uint32_t fid : fids) {
                if (fids_to_remove.find(fid) == fids_to_remove.end()) {
                    new_fids.insert(fid);
                }
            }
            if (new_fids.empty()) {
                store_.del("trigrams", key);
            } else {
                store_.put("trigrams", key, serialize_file_ids(new_fids));
            }
        }

        // 从 files 和 paths 移除（删除的文件）
        for (const auto& [p, fid] : deleted_files) {
            store_.del("files", serialize_u32_be(fid));
            store_.del("paths", wstring_to_utf8(p.wstring()));
        }
    }

    // 处理变更：重新索引（复用原 file_id）；变成二进制的 → 移除索引条目
    size_t became_binary = 0;
    for (const auto& [p, fid] : changed_files) {
        if (!index_single_file(p, fid)) {
            // 上方批量阶段已清掉其 postings/trigrams，这里补删 files/paths
            remove_file_from_index(fid, p);
            ++became_binary;
        }
    }

    // 处理新增：分配新 file_id；非文本文件不占号（返回 false 即未写任何条目）
    size_t indexed_new = 0;
    for (const auto& p : new_files) {
        if (index_single_file(p, max_fid + 1)) {
            ++max_fid;
            ++indexed_new;
        }
    }

    // 更新 meta
    auto files_keys = store_.get_keys("files");
    store_.put("meta", "nfiles",
               serialize_u32_le(static_cast<uint32_t>(files_keys.size())));
    store_.put("meta", "built_unix", serialize_i64_le(
        static_cast<int64_t>(std::time(nullptr))));

    // 提交
    if (!store_.commit()) return false;

    // 变更数上报（删除 + 修改 + 实际入索引的新增；被跳过的二进制文件不算变更）
    if (changed_count) {
        *changed_count = deleted_files.size() + changed_files.size() + indexed_new;
    }
    // 索引文件已重写（mtime 变）→ 作废缓存实例，下次 open 重新映射
    invalidate_cache(root_);
    global_query_cache().clear();   // A2 双保险：增量更新后查询缓存必须 miss
    logger::info(L"索引增量更新: 删除 " + std::to_wstring(deleted_files.size()) +
                 L" / 修改 " + std::to_wstring(changed_files.size()) +
                 L" / 新增 " + std::to_wstring(indexed_new) +
                 L" / 跳过二进制 " +
                 std::to_wstring(new_files.size() - indexed_new + became_binary));
    return true;
}

// ---- index_exists ----

bool Indexer::index_exists(const std::filesystem::path& root) {
    std::filesystem::path idx_path = root / ".textsearch" / "index.lmdb";
    // stat mtime+size（一次系统调用，远快于全文件 verify）
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(idx_path, ec);
    auto sz = std::filesystem::file_size(idx_path, ec);
    if (ec) return false;  // 文件不存在 / stat 失败 → 不查缓存直接 false

    std::wstring key = index_cache_key(root);
    // 快速路径：mtime+size 未变 → 复用上次结果
    {
        auto& cache = index_exists_cache();
        std::lock_guard<std::mutex> lk(cache.mtx);
        auto it = cache.map.find(key);
        if (it != cache.map.end()
            && it->second.mtime == mt
            && it->second.size == sz) {
            return it->second.exists;
        }
    }
    // 慢路径：open + verify（读全文件算 checksum）
    IndexStore store;
    if (!store.open(idx_path)) {
        // open 失败可能是瞬时锁/权限，不缓存 false 以免误判
        return false;
    }
    bool ok = store.verify();
    // 内容版本校验：索引文件格式没变，但里面存的内容口径变了（如 v2 的
    // 下划线子词补记）。版本不符 → 当作索引不存在，让上层重建。
    // 否则旧索引 checksum 依然正确，会一直被当成有效索引使用，继续漏报。
    if (ok && read_meta_version(store) != kIndexContentVersion) {
        ok = false;
    }
    {
        auto& cache = index_exists_cache();
        std::lock_guard<std::mutex> lk(cache.mtx);
        auto& e = cache.map[key];
        e.mtime = mt;
        e.size = sz;
        e.exists = ok;
    }
    return ok;
}

// ---- open_cached（Indexer 实例级缓存，避免每次搜索重新 load 全索引）----

namespace {
struct IndexerInstanceCache {
    std::mutex mtx;
    std::shared_ptr<Indexer> instance;
    std::filesystem::path root;
    std::filesystem::file_time_type mtime{};
    bool valid = false;
};
IndexerInstanceCache& indexer_instance_cache() {
    static IndexerInstanceCache c;
    return c;
}
} // namespace

std::shared_ptr<Indexer> Indexer::open_cached(const std::filesystem::path& root) {
    std::filesystem::path idx_path = root / ".textsearch" / "index.lmdb";
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(idx_path, ec);
    if (ec) {
        // 索引文件不存在 → 作废缓存并返回空
        invalidate_cache(root);
        return nullptr;
    }
    auto& c = indexer_instance_cache();
    std::lock_guard<std::mutex> lk(c.mtx);
    if (c.valid && c.root == root && c.mtime == mt && c.instance
        && c.instance->is_available()) {
        return c.instance;  // 命中：复用已 load 的内存索引
    }
    // 未命中：open 新实例（全文件 load）
    auto idx = std::make_shared<Indexer>();
    if (!idx->open(root)) {
        c.instance.reset();
        c.valid = false;
        return nullptr;
    }
    c.instance = idx;
    c.root = root;
    c.mtime = mt;
    c.valid = true;
    return c.instance;
}

void Indexer::invalidate_cache(const std::filesystem::path& root) {
    auto& c = indexer_instance_cache();
    std::lock_guard<std::mutex> lk(c.mtx);
    if (c.root == root || root.empty()) {
        c.instance.reset();
        c.valid = false;
    }
}

// ---- verify_and_recover（F.2.5）----

bool Indexer::verify_and_recover(const std::filesystem::path& root) {
    std::filesystem::path idx_path = root / ".textsearch" / "index.lmdb";
    if (!std::filesystem::exists(idx_path)) return false;

    IndexStore store;
    store.open(idx_path);
    if (!store.verify()) {
        // 损坏 → 删除索引文件（调用方降级即时模式）
        std::error_code ec;
        std::filesystem::remove_all(root / ".textsearch", ec);
        return false;
    }
    return true;
}
