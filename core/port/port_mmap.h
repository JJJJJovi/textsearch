// core/port/port_mmap.h — 只读文件滑动窗口映射移植接口（C1）
//
// 收口 core 三处各自手写的 Win32 MMF 窗口滑动（scanner.cpp scan_big_file、
// indexer.cpp for_each_line_mmap、mmap_index.cpp 整文件映射）。
// 一次仅保留一个活跃窗口；下一次 map()/close()/析构自动释放上一个窗口。
// offset 允许任意值——实现内部自行处理平台对齐限制（Windows 64KB 分配粒度），
// 调用方无需关心。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace port {

class SlidingFileMap {
public:
    SlidingFileMap();
    ~SlidingFileMap();

    SlidingFileMap(const SlidingFileMap&) = delete;
    SlidingFileMap& operator=(const SlidingFileMap&) = delete;

    // 只读打开（路径规整由实现负责，如 Windows 长路径前缀）。
    bool open(const std::filesystem::path& path);
    bool is_open() const;

    // 文件大小（字节）。未打开返回 0。
    uint64_t size() const;

    // 映射 [offset, offset+len) 只读窗口，返回窗口首指针；失败返回 nullptr
    // （同时释放上一个窗口）。指针在下一次 map()/close() 前有效。
    const unsigned char* map(uint64_t offset, size_t len);

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace port
