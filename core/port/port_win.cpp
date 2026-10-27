// core/port/port_win.cpp — 移植层 Windows 实现（C1）
//
// 代码全部搬自原先散落在 util.cpp / encoding.cpp / index_store.cpp /
// scanner.cpp / indexer.cpp / mmap_index.cpp 的 Win32 调用，行为不变。
// 本文件是 core 里除各 .rc/manifest 外**唯一**允许出现 windows.h 的地方。
#include "core/port/port_codec.h"
#include "core/port/port_fs.h"
#include "core/port/port_mmap.h"

#include <system_error>

#if defined(_WIN32) && !defined(TEXTSEARCH_PORT_STUB)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace port {

// ---- codec ----

static UINT to_cp(Codec c) { return c == Codec::Gbk ? 936 : CP_UTF8; }

bool mb_to_wide(const char* data, size_t len, Codec codec, bool strict,
                std::wstring& out) {
    if (len == 0) {
        out.clear();
        return true;
    }
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int wlen = MultiByteToWideChar(to_cp(codec), flags, data,
                                   static_cast<int>(len), nullptr, 0);
    if (wlen <= 0) return false;
    out.assign(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(to_cp(codec), flags, data, static_cast<int>(len),
                        out.data(), wlen);
    return true;
}

bool wide_to_utf8(const wchar_t* data, size_t len, std::string& out) {
    if (len == 0) {
        out.clear();
        return true;
    }
    int nlen = WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len),
                                   nullptr, 0, nullptr, nullptr);
    if (nlen <= 0) return false;
    out.assign(static_cast<size_t>(nlen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, static_cast<int>(len), out.data(),
                        nlen, nullptr, nullptr);
    return true;
}

// ---- fs ----

std::wstring to_long_path(const std::wstring& path) {
    // 超过 MAX_PATH 且未已有 \\?\ 前缀时添加
    if (path.size() > MAX_PATH &&
        path.compare(0, 4, L"\\\\?\\") != 0 &&
        path.compare(0, 8, L"\\\\?\\UNC\\") != 0) {
        // UNC 路径: \\server\share\... → \\?\UNC\server\share\...
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
            return L"\\\\?\\UNC\\" + path.substr(2);
        }
        return L"\\\\?\\" + path;
    }
    return path;
}

bool is_reparse_point(const std::filesystem::path& p) {
    // 先用 C++17 is_symlink 检测符号链接
    std::error_code ec;
    if (std::filesystem::is_symlink(p, ec)) {
        return true;
    }
    // 再用 GetFileAttributesW 检测 reparse point（含 junction）
    std::wstring path_str = to_long_path(p.wstring());
    DWORD attrs = GetFileAttributesW(path_str.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
    return false;
}

bool set_hidden(const std::filesystem::path& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return SetFileAttributesW(dir.c_str(), attr | FILE_ATTRIBUTE_HIDDEN) != 0;
}

std::filesystem::path app_data_dir() {
    const wchar_t* env = _wgetenv(L"LOCALAPPDATA");
    std::filesystem::path base;
    if (env && env[0] != L'\0') {
        base = std::filesystem::path(env);
    } else {
        base = std::filesystem::current_path();
    }
    std::filesystem::path app_dir = base / L"TextSearch";
    std::error_code ec;
    std::filesystem::create_directories(app_dir, ec);
    return app_dir;
}

// ---- mmap ----

struct SlidingFileMap::Impl {
    HANDLE hFile = nullptr;
    HANDLE hMap = nullptr;
    uint64_t fsize = 0;
    void* view = nullptr;        // 当前 MapViewOfFile 基址
    uint64_t view_off = 0;       // view 对应的文件偏移（已对齐到分配粒度）
    size_t view_len = 0;
    DWORD granularity = 65536;   // MapViewOfFile 偏移对齐粒度

    void unmap_view() {
        if (view) { UnmapViewOfFile(view); view = nullptr; }
        view_off = 0;
        view_len = 0;
    }
};

SlidingFileMap::SlidingFileMap() : impl_(std::make_unique<Impl>()) {}

SlidingFileMap::~SlidingFileMap() { close(); }

bool SlidingFileMap::open(const std::filesystem::path& path) {
    close();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwAllocationGranularity > 0)
        impl_->granularity = si.dwAllocationGranularity;

    std::wstring lp = to_long_path(path.wstring());
    impl_->hFile = CreateFileW(lp.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (impl_->hFile == INVALID_HANDLE_VALUE) {
        impl_->hFile = nullptr;
        return false;
    }
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(impl_->hFile, &fs) || fs.QuadPart <= 0) {
        close();
        return false;
    }
    impl_->fsize = static_cast<uint64_t>(fs.QuadPart);

    impl_->hMap = CreateFileMappingW(impl_->hFile, nullptr, PAGE_READONLY, 0, 0,
                                     nullptr);
    if (!impl_->hMap) {
        close();
        return false;
    }
    return true;
}

bool SlidingFileMap::is_open() const { return impl_->hMap != nullptr; }

uint64_t SlidingFileMap::size() const { return impl_->fsize; }

const unsigned char* SlidingFileMap::map(uint64_t offset, size_t len) {
    if (!impl_->hMap) return nullptr;
    if (offset >= impl_->fsize || len == 0) return nullptr;
    if (offset + len > impl_->fsize) len = static_cast<size_t>(impl_->fsize - offset);

    // MapViewOfFile 的偏移必须对齐到系统分配粒度：向下取整，返回指针补差值
    uint64_t base = offset - (offset % impl_->granularity);
    size_t extra = static_cast<size_t>(offset - base);
    size_t map_len = len + extra;

    impl_->unmap_view();
    DWORD off_hi = static_cast<DWORD>((base >> 32) & 0xFFFFFFFF);
    DWORD off_lo = static_cast<DWORD>(base & 0xFFFFFFFF);
    impl_->view = MapViewOfFile(impl_->hMap, FILE_MAP_READ, off_hi, off_lo,
                                map_len);
    if (!impl_->view) return nullptr;
    impl_->view_off = base;
    impl_->view_len = map_len;
    return static_cast<const unsigned char*>(impl_->view) + extra;
}

void SlidingFileMap::close() {
    if (!impl_) return;
    impl_->unmap_view();
    if (impl_->hMap) { CloseHandle(impl_->hMap); impl_->hMap = nullptr; }
    if (impl_->hFile) { CloseHandle(impl_->hFile); impl_->hFile = nullptr; }
    impl_->fsize = 0;
}

}  // namespace port

#endif  // _WIN32 && !TEXTSEARCH_PORT_STUB
