// core/index_store.cpp — 简易文件式 KV 存储实现（LMDB 替代）
#include "core/index_store.h"
#include "core/port/port_fs.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

// ---- 二进制读写辅助（小端）----

static void write_u16(std::ostream& os, uint16_t v) {
    char buf[2];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    os.write(buf, 2);
}

static void write_u32(std::ostream& os, uint32_t v) {
    char buf[4];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    buf[2] = static_cast<char>((v >> 16) & 0xFF);
    buf[3] = static_cast<char>((v >> 24) & 0xFF);
    os.write(buf, 4);
}

static bool read_u16(std::istream& is, uint16_t& out) {
    char buf[2];
    is.read(buf, 2);
    if (!is || is.gcount() != 2) return false;
    out = static_cast<uint16_t>(static_cast<uint8_t>(buf[0])) |
          (static_cast<uint16_t>(static_cast<uint8_t>(buf[1])) << 8);
    return true;
}

static bool read_u32(std::istream& is, uint32_t& out) {
    char buf[4];
    is.read(buf, 4);
    if (!is || is.gcount() != 4) return false;
    out = static_cast<uint32_t>(static_cast<uint8_t>(buf[0])) |
          (static_cast<uint32_t>(static_cast<uint8_t>(buf[1])) << 8) |
          (static_cast<uint32_t>(static_cast<uint8_t>(buf[2])) << 16) |
          (static_cast<uint32_t>(static_cast<uint8_t>(buf[3])) << 24);
    return true;
}

// ---- FNV-1a 32 位校验和 ----
// 对所有子库的 key + value 字节做哈希，保证数据完整性可校验。
// 遍历顺序无关：对 db_name 和 key 先排序再哈希，保证 save 和 load 时一致。
uint32_t IndexStore::compute_checksum() const {
    uint32_t hash = 2166136261u;  // FNV offset basis
    const uint32_t prime = 16777619u;
    // 收集所有 db_name 并排序（保证遍历顺序一致）
    std::vector<std::string> db_names;
    db_names.reserve(dbs_.size());
    for (const auto& [db_name, entries] : dbs_) {
        db_names.push_back(db_name);
    }
    std::sort(db_names.begin(), db_names.end());
    for (const auto& db_name : db_names) {
        for (char c : db_name) {
            hash ^= static_cast<uint8_t>(c);
            hash *= prime;
        }
        hash ^= 0x01u;
        hash *= prime;
        // 收集该 db 的所有 key 并排序
        const auto& entries = dbs_.at(db_name);
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (const auto& [k, v] : entries) {
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            for (char c : key) {
                hash ^= static_cast<uint8_t>(c);
                hash *= prime;
            }
            hash ^= 0x02u;
            hash *= prime;
            const std::vector<uint8_t>& val = entries.at(key);
            for (uint8_t b : val) {
                hash ^= b;
                hash *= prime;
            }
            hash ^= 0x03u;
            hash *= prime;
        }
    }
    return hash;
}

// ---- 序列化（写盘）----
// 文件格式：
//   [magic: 4B "TSID"]
//   [version: uint32]
//   [checksum: uint32]   // 对所有子库数据的 FNV-1a
//   [num_dbs: uint32]
//     for each db:
//       [name_len: uint16][name: bytes]
//       [num_entries: uint32]
//       for each entry:
//         [key_len: uint16][key: bytes]
//         [val_len: uint32][val: bytes]
namespace {

// O8：把索引目录（<root>/.textsearch）标为隐藏，降低被同步盘 / Git 误收录的
// 概率。只碰索引目录自身，绝不动用户的搜索根目录——否则会把人家整个工程隐藏。
// 失败也无所谓（FAT/exFAT、只读介质都可能失败），不影响索引功能，故不上抛错误。
void mark_index_dir_hidden(const std::filesystem::path& dir) {
    port::set_hidden(dir);   // 非 Windows 平台为 no-op（点开头即隐藏）
}

} // namespace

bool IndexStore::save() const {
    // 确保父目录存在
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    mark_index_dir_hidden(path_.parent_path());   // O8

    std::ofstream ofs(path_, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;

    // magic
    ofs.write("TSID", 4);
    // version
    write_u32(ofs, 2u);
    // checksum
    write_u32(ofs, compute_checksum());
    // num_dbs
    write_u32(ofs, static_cast<uint32_t>(dbs_.size()));

    for (const auto& [db_name, entries] : dbs_) {
        // db name
        uint16_t name_len = static_cast<uint16_t>(db_name.size());
        write_u16(ofs, name_len);
        if (name_len > 0) {
            ofs.write(db_name.data(), name_len);
        }
        // num_entries
        write_u32(ofs, static_cast<uint32_t>(entries.size()));
        for (const auto& [key, val] : entries) {
            uint16_t key_len = static_cast<uint16_t>(key.size());
            write_u16(ofs, key_len);
            if (key_len > 0) {
                ofs.write(key.data(), key_len);
            }
            uint32_t val_len = static_cast<uint32_t>(val.size());
            write_u32(ofs, val_len);
            if (val_len > 0) {
                ofs.write(reinterpret_cast<const char*>(val.data()),
                          static_cast<std::streamsize>(val_len));
            }
        }
    }
    return ofs.good();
}

// ---- 反序列化（读盘）----
bool IndexStore::load() {
    std::ifstream ifs(path_, std::ios::binary);
    if (!ifs) return false;

    // magic
    char magic[4];
    ifs.read(magic, 4);
    if (!ifs || ifs.gcount() != 4 || std::memcmp(magic, "TSID", 4) != 0) {
        return false;
    }

    // version（=2：postings/file_ids 改 varint+delta 压缩格式；旧索引 v1 直接拒绝 → 重建）
    uint32_t version = 0;
    if (!read_u32(ifs, version) || version != 2) return false;

    // checksum
    uint32_t stored_checksum = 0;
    if (!read_u32(ifs, stored_checksum)) return false;

    // num_dbs
    uint32_t num_dbs = 0;
    if (!read_u32(ifs, num_dbs)) return false;

    dbs_.clear();
    for (uint32_t d = 0; d < num_dbs; ++d) {
        // db name
        uint16_t name_len = 0;
        if (!read_u16(ifs, name_len) || name_len > 4096) return false;
        std::string db_name(name_len, '\0');
        if (name_len > 0) {
            ifs.read(&db_name[0], name_len);
            if (!ifs || static_cast<uint32_t>(ifs.gcount()) != name_len) return false;
        }

        // num_entries
        uint32_t num_entries = 0;
        if (!read_u32(ifs, num_entries)) return false;

        auto& entries = dbs_[db_name];
        for (uint32_t e = 0; e < num_entries; ++e) {
            uint16_t key_len = 0;
            if (!read_u16(ifs, key_len)) return false;
            std::string key(key_len, '\0');
            if (key_len > 0) {
                ifs.read(&key[0], key_len);
                if (!ifs || static_cast<uint32_t>(ifs.gcount()) != key_len) return false;
            }

            uint32_t val_len = 0;
            if (!read_u32(ifs, val_len) || val_len > 256u * 1024 * 1024) return false;
            std::vector<uint8_t> val(val_len);
            if (val_len > 0) {
                ifs.read(reinterpret_cast<char*>(val.data()),
                         static_cast<std::streamsize>(val_len));
                if (!ifs || static_cast<uint32_t>(ifs.gcount()) != val_len) return false;
            }
            entries[std::move(key)] = std::move(val);
        }
    }

    // 校验 checksum（重新计算并比对）
    uint32_t computed = compute_checksum();
    if (computed != stored_checksum) {
        return false;
    }

    return true;
}

// ---- open / close ----

bool IndexStore::open(const std::filesystem::path& path) {
    path_ = path;
    opened_ = true;
    if (std::filesystem::exists(path)) {
        return load();
    }
    // 新文件：dbs_ 为空
    dbs_.clear();
    return true;
}

void IndexStore::close() {
    dbs_.clear();
    path_.clear();
    opened_ = false;
}

// ---- 子库操作 ----

void IndexStore::put(const std::string& db, const std::string& key,
                     const std::vector<uint8_t>& val) {
    dbs_[db][key] = val;
}

bool IndexStore::get(const std::string& db, const std::string& key,
                     std::vector<uint8_t>& out) const {
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return false;
    auto kit = dit->second.find(key);
    if (kit == dit->second.end()) return false;
    out = kit->second;
    return true;
}

const std::vector<uint8_t>* IndexStore::get(const std::string& db,
                                             const std::string& key) const {
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return nullptr;
    auto kit = dit->second.find(key);
    if (kit == dit->second.end()) return nullptr;
    return &kit->second;
}

void IndexStore::del(const std::string& db, const std::string& key) {
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return;
    dit->second.erase(key);
}

bool IndexStore::has(const std::string& db, const std::string& key) const {
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return false;
    return dit->second.find(key) != dit->second.end();
}

std::vector<std::string> IndexStore::get_keys(const std::string& db) const {
    std::vector<std::string> keys;
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return keys;
    keys.reserve(dit->second.size());
    for (const auto& [k, v] : dit->second) {
        keys.push_back(k);
    }
    return keys;
}

std::vector<std::pair<std::string, std::vector<uint8_t>>>
IndexStore::get_all(const std::string& db) const {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> all;
    auto dit = dbs_.find(db);
    if (dit == dbs_.end()) return all;
    all.reserve(dit->second.size());
    for (const auto& [k, v] : dit->second) {
        all.emplace_back(k, v);
    }
    return all;
}

// ---- commit ----

bool IndexStore::commit() {
    if (!opened_ || path_.empty()) return false;
    return save();
}

// ---- verify ----

bool IndexStore::verify() const {
    if (path_.empty() || !std::filesystem::exists(path_)) return false;
    // 完整 load 到临时对象校验 magic + checksum
    IndexStore tmp;
    tmp.path_ = path_;
    return tmp.load();
}
