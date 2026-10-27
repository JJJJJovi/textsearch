// core/indexer.h — 倒排索引接口（T4b）
// 简易文件式 KV 存储（IndexStore）替代 LMDB，实现完整倒排索引。
// 支持：子串搜索（bi-gram + 邻接验证 F.2.2）、正则搜索（trigram 加速 E.3）、
//       增量更新（mtime/size 对比）、损坏恢复（F.2.5 checksum）。
#pragma once

#include "core/index_store.h"
#include "core/mmap_index.h"
#include "core/types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

// 倒排索引器（简易文件式 KV 存储实现）
class Indexer {
public:
    Indexer();
    ~Indexer();

    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    // 检查目录是否已建索引（且 verify() 通过）
    static bool index_exists(const std::filesystem::path& root);

    // 建立索引（全量构建）。
    // B3：on_progress（可选）在建库过程中被调用（done 递增至 total；初始一次
    // (0, total)）。**并行 worker 线程会并发调用**，回调自身须线程安全
    // （GUI 侧实现为 PostMessage，天然满足）。传 nullptr 行为与旧版一致。
    using BuildProgress = std::function<void(size_t done, size_t total)>;
    bool build(const std::filesystem::path& root,
               const std::set<std::wstring>& ext_set,
               const BuildProgress& on_progress = nullptr);

    // 索引搜索（子串/正则，根据 opts.use_regex 路由）
    // - ext_set: 已解析的最终扩展名集合；非空时仅返回这些扩展名的结果，
    //            与 filesystem.cpp 的 enumerate_files 口径一致（含前导点、小写）。
    //            空集表示不过滤（返回索引内全部命中）。带默认值以兼容旧调用。
    std::vector<MatchResult> search(const std::wstring& pattern,
                                    const SearchOptions& opts,
                                    const std::set<std::wstring>& ext_set = {});

    // 返回建库时纳入索引的扩展名集合（读 meta.ext_set）。
    // 用途：B3 —— 用户勾选了索引里没有的类型时，结果恒为空且界面无提示（静默错误）。
    // 调用方可据此与用户勾选集合比对，提示「N 种类型不在索引内，建议重建索引」。
    // 索引未加载 / 无该 meta 时返回空集（调用方须把空集当作「无法判断」处理）。
    std::set<std::wstring> indexed_ext_set() const;

    // 增量更新（检测 mtime/size 变更，补建/重建/清除）
    // changed_count（可选出参）：本次实际变更的文件数（删除 + 修改 + 新增）。
    // 无变化时返回 true 且 changed_count = 0（走快速预检，不触发全量 load / save）。
    bool update_incremental(size_t* changed_count = nullptr);

    // 损坏检测与恢复（F.2.5：校验 checksum，失败则删除索引文件）
    static bool verify_and_recover(const std::filesystem::path& root);

    // 大文件阈值（MB）：单文件超过此值走 MMF 流式索引（内存有界，避免 OOM）。
    // 与 Scanner 的 bigfile_threshold_mb 默认一致（50）。GUI 建索引前可经
    // set_bigfile_threshold_mb 传入用户配置值；不传则用默认 50。
    void set_bigfile_threshold_mb(uint64_t mb) { bigfile_threshold_mb_ = mb ? mb : 1; }
    uint64_t bigfile_threshold_mb() const { return bigfile_threshold_mb_; }

    // 缓存已打开的 Indexer（按 root + 索引文件 mtime）。
    // 重复搜索同一目录时复用，省掉每次 open() 的全文件 load（IndexStore::open
    // 会把整个索引读进内存 map）。线程安全；同一实例不可被并发 search
    // （GUI 单搜索串行，满足）。索引文件 mtime 变化（重建/增量）即 cache miss。
    static std::shared_ptr<Indexer> open_cached(const std::filesystem::path& root);
    // 作废指定 root 的缓存（build / 增量更新后调用）
    static void invalidate_cache(const std::filesystem::path& root);

    // 是否可用（索引已加载）
    bool is_available() const { return available_; }

    // 打开已有索引（供 search/update 使用）
    bool open(const std::filesystem::path& root);

private:
    // ---- 文件记录 ----
    struct FileRecord {
        std::string path;   // UTF-8
        int64_t mtime = 0;
        int64_t size = 0;
        uint8_t enc = 0;    // 0=utf8, 1=gbk, 2=utf16le, 3=utf16be
    };

    // ---- 查询词项 ----
    struct PatternToken {
        std::wstring term;   // 词项文本
        int offset = 0;      // 在 pattern 中的起始字符位置（0-indexed）
        int length = 0;      // 词项字符数
    };

    // ---- 成员 ----
    std::filesystem::path root_;
    std::filesystem::path index_path_;
    bool available_ = false;
    uint64_t bigfile_threshold_mb_ = 50;        // 大文件 MMF 流式阈值（MB）
    IndexStore store_;                          // 写路径（build/update_incremental）
    std::unique_ptr<MmapIndexReader> reader_;   // 读路径（mmap 零拷贝，search 用）

    // 统一读访问：优先 mmap reader_（零拷贝），无则回退 store_.get。
    // 返回指向索引数据的指针 + 长度；未命中返回 nullptr。
    // 生命周期：mmap 模式下随 reader_ 存活；store_ 模式下随 store_ 存活。
    const uint8_t* get_blob(const std::string& db, const std::string& key,
                            size_t& len) const;

    // ---- 索引路径计算 ----
    // <root>/.textsearch/index.lmdb
    std::filesystem::path compute_index_path(
        const std::filesystem::path& root) const;

    // ---- 词项切分 ----
    // 切分一行文本为词项列表 [(term, col), ...]
    // ASCII: [A-Za-z0-9_]+ 连续为一个 token
    // CJK: 每两个相邻 CJK 字符为一个 bi-gram token
    // 标点/空白: 分隔符
    std::vector<std::pair<std::wstring, int>>
    tokenize_line(const std::wstring& line) const;

    // bi-gram 切分（CJK）："中文文本" → 中文, 文文, 文本
    std::vector<std::wstring> bigram_cjk(const std::wstring& s) const;

    // 切分 pattern 为词项 + 偏移信息（用于邻接验证）
    std::vector<PatternToken>
    tokenize_pattern(const std::wstring& pattern) const;

    // ---- trigram 提取 ----
    // 从一行文本提取字符级 3-gram（文件级去重由调用方处理）
    std::set<std::string> extract_trigrams(const std::wstring& line,
                                           bool ignore_case) const;

    // ---- 正则字面量段提取 ----
    // 按顶层 '|' 分支分组返回（每分支一组，组内为长度≥3 的必选字面量段）。
    // 候选集语义：组内 run 取交集，分支间取并集。
    // 保守简化（宁慢不漏）：含 '|' 的括号组、被 ?/*/{0,n} 量化的组，
    // 整组不参与筛选；字符类 [...] 整体跳过（不产生字面量）；
    // 被 ?/*/{0,n} 量化的字面字符从 run 尾部剔除。
    std::vector<std::vector<std::wstring>>
    extract_literal_runs(const std::wstring& pattern) const;

    // ---- 序列化/反序列化 ----
    // postings: varint+delta（v2 压缩格式）
    std::vector<uint8_t> serialize_postings(
        const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>& p) const;
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
    deserialize_postings(const std::vector<uint8_t>& data) const;
    // (ptr, len) 重载：mmap 零拷贝路径用，避免先构造 vector 再拷贝
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>
    deserialize_postings(const uint8_t* data, size_t len) const;

    // trigrams: varint+delta（v2 压缩格式）
    std::vector<uint8_t> serialize_file_ids(
        const std::set<uint32_t>& ids) const;
    std::set<uint32_t> deserialize_file_ids(
        const std::vector<uint8_t>& data) const;
    std::set<uint32_t> deserialize_file_ids(
        const uint8_t* data, size_t len) const;

    // FileRecord 序列化:
    //   [path_len: uint16][path: UTF-8][mtime: int64 LE][size: int64 LE][enc: uint8]
    std::vector<uint8_t> serialize_file_record(const FileRecord& rec) const;
    bool deserialize_file_record(const std::vector<uint8_t>& data,
                                 FileRecord& out) const;
    bool deserialize_file_record(const uint8_t* data, size_t len,
                                 FileRecord& out) const;

    // uint32 大端序列化（file_id key）
    std::string serialize_u32_be(uint32_t v) const;

    // uint32 小端序列化（value）
    std::vector<uint8_t> serialize_u32_le(uint32_t v) const;
    // int64 小端序列化（value）
    std::vector<uint8_t> serialize_i64_le(int64_t v) const;

    // ---- 编码映射 ----
    uint8_t encoding_to_uint8(Encoding enc) const;
    Encoding uint8_to_encoding(uint8_t v) const;

    // ---- CJK 判定 ----
    bool is_cjk(wchar_t c) const;

    // ---- meta.ext_set 反序列化 ----
    // 格式：[count:u32 LE][len:u16 LE][utf8 bytes] × count
    // 原本在 update_incremental 里有两份重复实现（blob 版 / vector 版），
    // 抽成一个 helper 避免解析逻辑漂移。
    std::set<std::wstring> parse_meta_ext_set(const uint8_t* p, size_t len) const;

    // ---- 单文件索引（供 update_incremental 使用）----
    // 返回 false = 非文本（is_textual 探测不过），未写入任何索引条目；
    // 调用方（update_incremental）据此不分配 file_id / 移除旧条目。
    bool index_single_file(const std::filesystem::path& path,
                           uint32_t file_id);

    // 移除某文件的全部索引条目（postings / trigrams / files / paths）。
    // 用于变更文件变成二进制的场景（update_incremental）。
    void remove_file_from_index(uint32_t fid,
                                const std::filesystem::path& path);

    // MMF 流式逐行读取（大文件索引与 build 大文件路径共用）。
    // 滑动窗口、仅在换行边界处切行（UTF-8/GBK 按字节 0x0A；B1 起 UTF-16 按
    // 偶对齐 2 字节码元 0A00/000A），每行回调 on_line(line_no, line)，line 已
    // 去 \r、line_no 从 1 起。打开/映射失败返回 false（调用方决定跳过或回退
    // 策略，绝不整文件读入）。
    bool for_each_line_mmap(
        const std::filesystem::path& path, Encoding enc,
        const std::function<void(int line_no, const std::wstring& line)>&
            on_line) const;

    // 大文件 MMF 流式索引（size > bigfile_threshold_mb_，含 UTF-16——B1 起流式
    // 切行覆盖 UTF-16，不再回退整文件读入的内存路径）。
    // postings 攒到 kPostingFlushCap 即 flush 回 store，内存有界，杜绝整文件
    // 读入导致的 OOM。MMF 不可用时记日志并跳过该文件（绝不回退 read_text，
    // 否则大文件仍会 OOM）。
    void index_single_file_large(const std::filesystem::path& path,
                                 uint32_t file_id, Encoding enc);

    // 把累加器里某 term 的 postings 合并写回 store（读 → 追加 → 排序 → 写）。
    void flush_postings(
        std::unordered_map<std::string,
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>>& acc);

    // ---- 回读文件指定行 ----
    std::wstring read_line(const std::filesystem::path& path,
                           int line_no) const;

    // ---- 搜索子方法 ----
    std::vector<MatchResult> search_substring(
        const std::wstring& pattern, const SearchOptions& opts);
    std::vector<MatchResult> search_regex(
        const std::wstring& pattern, const SearchOptions& opts);

    // 回退全文件搜索（pattern 无法 tokenize 时）
    std::vector<MatchResult> search_fallback_all(
        const std::wstring& pattern, const SearchOptions& opts);

    // 获取索引中所有 file_id
    std::set<uint32_t> get_all_file_ids() const;
};

// 大文件流式索引的映射窗口大小（默认 64MB）。仅测试注入小窗口用
// （B1：小窗口才能在单测里覆盖 UTF-16 多窗口滑动/码元边界拆分）；
// 非线程安全，请在无索引构建运行时设置，用完恢复。
void set_index_mmap_window(size_t n);
size_t get_index_mmap_window();
