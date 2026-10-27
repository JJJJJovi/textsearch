// core/mmap_index.h — 索引文件 mmap 只读视图（零分配 open）
//
// 设计动机：IndexStore::open 用 ifstream 把整个索引读进
// unordered_map<string, unordered_map<string, vector<uint8_t>>>，
// 每条 key/value 都是一次堆分配。50MB 索引 = 几百万次 new，
// open 时间被分配器吃掉，而非 I/O。
//
// 本类把索引文件 mmap 进进程地址空间（一次系统调用），建一张轻量查找表
// (db, key) → (offset, len)，key 仍拷贝（短字符串，开销小），value 只存
// offset+len 不拷贝。get() 返回指向 mmap 内存的裸指针，零拷贝、零分配。
//
// 仅读：不支持 put/del/commit。写路径仍用 IndexStore（build/update_incremental）。
// 磁盘格式与 IndexStore v2 完全一致，两者读同一份文件。
//
// 校验：open 时遍历所有 value 字节算 FNV-1a 与文件头 checksum 比对
// （和 IndexStore::load + verify 同口径）。校验失败返回 false。
//
// 生命周期：映射在对象存活期间有效；析构时释放（经 core/port 的 SlidingFileMap，
// Windows 为 MMF，其他平台为堆缓冲整文件读——C1 移植层）。
// 调用方持有 MmapIndexReader 期间，索引文件不得被外部修改/删除
// （build 会先 close reader 再写，见 Indexer）。
#pragma once

#include "core/port/port_mmap.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 索引文件 mmap 只读视图
class MmapIndexReader {
public:
    MmapIndexReader() = default;
    ~MmapIndexReader();

    MmapIndexReader(const MmapIndexReader&) = delete;
    MmapIndexReader& operator=(const MmapIndexReader&) = delete;

    // 打开并映射索引文件。成功返回 true。
    // 失败（文件不存在/magic 错/version 不符/checksum 不符）返回 false。
    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const { return base_ != nullptr; }

    // 取值：返回指向 mmap 内存的指针 + 长度。未命中返回 nullptr。
    // 返回的指针在本对象存活且未 close 期间有效。
    const uint8_t* get(const std::string& db, const std::string& key,
                       size_t& len) const;

    // 取子库所有 key（用于遍历 postings/trigrams，供增量更新枚举）
    std::vector<std::string> get_keys(const std::string& db) const;

private:
    // 查找表项：value 在 mmap 中的偏移 + 字节数
    struct Entry { size_t offset; size_t len; };

    std::unique_ptr<port::SlidingFileMap> map_;   // 只读整文件映射（C1 移植层）
    const uint8_t* base_ = nullptr;   // 映射基址（map_->map(0, size) 的返回值）
    size_t size_ = 0;

    // db_name → (key → Entry)。key 拷贝（短字符串），value 不拷贝。
    // 比 IndexStore 的 unordered_map<string, unordered_map<string, vector<uint8_t>>>
    // 省掉全部 value 的堆分配（几百万次 → 0）。
    std::unordered_map<std::string,
                       std::unordered_map<std::string, Entry>> index_;

    // 索引文件头解析（与 IndexStore::load 同格式，v2）
    bool parse_header_and_build_index();
    // FNV-1a 校验（与 IndexStore::compute_checksum 同口径）
    bool verify_checksum(uint32_t stored) const;
};
