// core/query_cache.cpp — A2 查询结果 LRU 缓存：进程级共享实例
#include "core/query_cache.h"

QueryCache& global_query_cache() {
    static QueryCache inst;   // 容量 8（kDefaultCapacity）
    return inst;
}
