// core/mmap_index.cpp — mmap 只读索引视图实现
#include "core/mmap_index.h"

#include <algorithm>
#include <cstring>
#include <vector>

// ---- 小端读取辅助（直接从 mmap 内存读，无 ifstream）----
namespace {
inline uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

MmapIndexReader::~MmapIndexReader() {
    close();
}

void MmapIndexReader::close() {
    base_ = nullptr;
    map_.reset();
    index_.clear();
    size_ = 0;
}

bool MmapIndexReader::open(const std::filesystem::path& path) {
    close();

    // 只读打开 + 整文件映射（经移植层；Windows 为 MMF 零分配）
    map_ = std::make_unique<port::SlidingFileMap>();
    if (!map_->open(path)) {
        map_.reset();
        return false;
    }
    size_ = static_cast<size_t>(map_->size());

    base_ = map_->map(0, size_);
    if (!base_) {
        close();
        return false;
    }

    if (!parse_header_and_build_index()) {
        close();
        return false;
    }
    return true;
}

bool MmapIndexReader::parse_header_and_build_index() {
    // 文件格式（与 IndexStore::save 一致，v2）：
    //   [magic: 4B "TSID"][version: u32=2][checksum: u32][num_dbs: u32]
    //   for each db: [name_len: u16][name][num_entries: u32]
    //     for each entry: [key_len: u16][key][val_len: u32][val]
    if (size_ < 16) return false;
    if (std::memcmp(base_, "TSID", 4) != 0) return false;
    uint32_t version = rd_u32(base_ + 4);
    if (version != 2) return false;
    uint32_t stored_checksum = rd_u32(base_ + 8);
    uint32_t num_dbs = rd_u32(base_ + 12);

    size_t pos = 16;
    index_.clear();
    index_.reserve(num_dbs);

    for (uint32_t d = 0; d < num_dbs; ++d) {
        // db name
        if (pos + 2 > size_) return false;
        uint16_t name_len = rd_u16(base_ + pos); pos += 2;
        if (name_len > 4096 || pos + name_len > size_) return false;
        std::string db_name(reinterpret_cast<const char*>(base_ + pos), name_len);
        pos += name_len;
        // num_entries
        if (pos + 4 > size_) return false;
        uint32_t num_entries = rd_u32(base_ + pos); pos += 4;

        auto& entries = index_[db_name];
        entries.reserve(num_entries);
        for (uint32_t e = 0; e < num_entries; ++e) {
            // key
            if (pos + 2 > size_) return false;
            uint16_t key_len = rd_u16(base_ + pos); pos += 2;
            if (pos + key_len > size_) return false;
            std::string key(reinterpret_cast<const char*>(base_ + pos), key_len);
            pos += key_len;
            // value
            if (pos + 4 > size_) return false;
            uint32_t val_len = rd_u32(base_ + pos); pos += 4;
            if (val_len > 256u * 1024 * 1024 || pos + val_len > size_) return false;
            // 记录 value 在 mmap 中的偏移（不拷贝内容）
            entries.emplace(std::move(key), Entry{pos, val_len});
            pos += val_len;
        }
    }

    // checksum 校验：遍历所有 db/key/value 字节算 FNV-1a，与 IndexStore 同口径
    if (!verify_checksum(stored_checksum)) return false;
    return true;
}

bool MmapIndexReader::verify_checksum(uint32_t stored) const {
    // FNV-1a 32-bit，遍历顺序：db_name 升序 → key 升序 → value 字节
    // 与 IndexStore::compute_checksum 完全一致，保证 save/load/mmap 三处同口径
    uint32_t hash = 2166136261u;
    const uint32_t prime = 16777619u;

    std::vector<std::string> db_names;
    db_names.reserve(index_.size());
    for (const auto& [db, _] : index_) db_names.push_back(db);
    std::sort(db_names.begin(), db_names.end());

    for (const auto& db_name : db_names) {
        for (char c : db_name) { hash ^= static_cast<uint8_t>(c); hash *= prime; }
        hash ^= 0x01u; hash *= prime;

        const auto& entries = index_.at(db_name);
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (const auto& [k, _] : entries) keys.push_back(k);
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys) {
            for (char c : key) { hash ^= static_cast<uint8_t>(c); hash *= prime; }
            hash ^= 0x02u; hash *= prime;
            const Entry& en = entries.at(key);
            for (size_t i = 0; i < en.len; ++i) {
                hash ^= base_[en.offset + i];
                hash *= prime;
            }
            hash ^= 0x03u; hash *= prime;
        }
    }
    return hash == stored;
}

const uint8_t* MmapIndexReader::get(const std::string& db, const std::string& key,
                                    size_t& len) const {
    auto dit = index_.find(db);
    if (dit == index_.end()) return nullptr;
    auto kit = dit->second.find(key);
    if (kit == dit->second.end()) return nullptr;
    len = kit->second.len;
    return base_ + kit->second.offset;
}

std::vector<std::string> MmapIndexReader::get_keys(const std::string& db) const {
    std::vector<std::string> keys;
    auto dit = index_.find(db);
    if (dit == index_.end()) return keys;
    keys.reserve(dit->second.size());
    for (const auto& [k, _] : dit->second) keys.push_back(k);
    return keys;
}
