// core/query_cache.h — A2 查询结果 LRU 缓存（独立可单测，TODO 2026-10-27）
// 索引搜索的重复查询（同 pattern + 同选项 + 同索引状态）直接返回上次结果，
// 省掉候选集求交 + 逐文件读入精筛的整条流水线。
//
// 键完整性原则：键必须「完整决定结果」——任何影响结果的因素缺席都会把
// 陈旧结果当成新结果返回。键字段清单（缺一即陈旧风险，tests/test_query_cache.cpp
// 有逐一反向验证）：
//   pattern + use_regex + ignore_case + whole_word + recursive
//   + root + ext_set（逐项拼接）
//   + 索引文件 mtime + size（调用方必须在 update_incremental 之后现取——
//     增量更新会改写索引文件，mtime/size 变化即自然 miss）
//   + kIndexContentVersion（索引内容口径升级后旧条目整体失效）
#pragma once

#include "core/types.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

class QueryCache {
public:
    // 缓存键。operator== 逐字段比较，一项都不能省（反向验证依赖）。
    struct Key {
        std::wstring pattern;
        bool use_regex = false;
        bool ignore_case = false;
        bool whole_word = false;
        bool recursive = true;
        std::wstring root;             // 索引根目录（决定结果全集）
        std::wstring ext_set_joined;   // ext_set 逐项拼接（std::set 有序 → 拼接确定）
        int64_t index_mtime = 0;       // 索引文件 mtime（unix 秒）
        int64_t index_size = -1;       // 索引文件字节数
        uint32_t content_version = 0;  // kIndexContentVersion

        bool operator==(const Key& o) const {
            return pattern == o.pattern
                && use_regex == o.use_regex
                && ignore_case == o.ignore_case
                && whole_word == o.whole_word
                && recursive == o.recursive
                && root == o.root
                && ext_set_joined == o.ext_set_joined
                && index_mtime == o.index_mtime
                && index_size == o.index_size
                && content_version == o.content_version;
        }
        bool operator!=(const Key& o) const { return !(*this == o); }
    };

    static constexpr size_t kDefaultCapacity = 8;

    explicit QueryCache(size_t capacity = kDefaultCapacity)
        : capacity_(capacity ? capacity : 1) {}

    // 查询：命中返回 true 并把结果拷入 out（同时 touch 到最新）。
    bool get(const Key& k, std::vector<MatchResult>& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == k) {
                if (it != entries_.begin()) {
                    entries_.splice(entries_.begin(), entries_, it);  // LRU touch
                }
                out = entries_.front().results;
                ++hits_;
                return true;
            }
        }
        ++misses_;
        return false;
    }

    // 写入。调用方负责把关「不缓存」条件（达 max_results 截断 / B3 告警路径）；
    // 满容量时淘汰最久未用条目。
    void put(const Key& k, std::vector<MatchResult> results) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->key == k) {           // 覆盖同键旧值并 touch
                it->results = std::move(results);
                entries_.splice(entries_.begin(), entries_, it);
                return;
            }
        }
        if (entries_.size() >= capacity_) {
            entries_.pop_back();
        }
        entries_.push_front({k, std::move(results)});
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return entries_.size();
    }

    // 命中/未命中计数（单测观测 hit/miss 用；单测须先 reset_stats 保证隔离）
    uint64_t hits() const { std::lock_guard<std::mutex> lk(mtx_); return hits_; }
    uint64_t misses() const { std::lock_guard<std::mutex> lk(mtx_); return misses_; }
    void reset_stats() {
        std::lock_guard<std::mutex> lk(mtx_);
        hits_ = 0;
        misses_ = 0;
    }

private:
    size_t capacity_;
    struct Entry { Key key; std::vector<MatchResult> results; };
    std::list<Entry> entries_;   // front = 最近使用，back = 最久未用
    // 索引查询目前为串行调用（GUI 单搜索线程），但缓存是进程级共享对象，
    // 防御性加锁——锁开销相对整条搜索流水线可忽略。
    mutable std::mutex mtx_;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

// 进程级共享实例。键含 root + 索引 mtime/size + 内容版本，多个 Indexer
// 实例共用不会串结果；索引文件重写后键必然变化，无需显式失效也能保证
// 正确（build/update_incremental 另有显式 clear 作双保险）。
QueryCache& global_query_cache();
