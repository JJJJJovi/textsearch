// core/index_store.h — 简易文件式 KV 存储（LMDB 替代）
// 内存中用 unordered_map，commit 时序列化为二进制文件。
// 支持命名子库（meta/paths/files/postings/trigrams）。
// 接口设计对标 LMDB 的子集，后续可无缝替换为真 LMDB。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 简易文件式 KV 存储（LMDB 替代）
// 内存中用 unordered_map，commit 时序列化为二进制文件。
// 支持命名子库（meta/paths/files/postings/trigrams）。
class IndexStore {
public:
    IndexStore() = default;
    ~IndexStore() = default;

    IndexStore(const IndexStore&) = delete;
    IndexStore& operator=(const IndexStore&) = delete;

    // 打开/创建索引文件。文件存在则加载，不存在则创建空存储。
    bool open(const std::filesystem::path& path);
    void close();

    // 子库操作
    void put(const std::string& db, const std::string& key,
             const std::vector<uint8_t>& val);
    // 取值（拷贝到 out）。命中返回 true。非热路径用。
    bool get(const std::string& db, const std::string& key,
             std::vector<uint8_t>& out) const;
    // 取值（返回内部值的 const 指针，不拷贝）。热路径（posting list 查询）用，
    // 避免每个 term 都拷贝整条 posting list 向量。未命中返回 nullptr。
    const std::vector<uint8_t>* get(const std::string& db,
                                    const std::string& key) const;
    void del(const std::string& db, const std::string& key);
    bool has(const std::string& db, const std::string& key) const;

    // 获取子库所有 key（用于遍历 postings/trigrams）
    std::vector<std::string> get_keys(const std::string& db) const;

    // 获取子库所有 key-value 对
    std::vector<std::pair<std::string, std::vector<uint8_t>>>
    get_all(const std::string& db) const;

    // 提交（刷盘）
    bool commit();

    // 完整性校验（F.2.5：简单校验和）
    bool verify() const;

    // 索引文件路径
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    // db_name → (key → value)
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::vector<uint8_t>>> dbs_;
    bool opened_ = false;

    bool load();
    bool save() const;
    uint32_t compute_checksum() const;
};
