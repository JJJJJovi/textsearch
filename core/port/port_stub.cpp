// core/port/port_stub.cpp — 移植层可移植 stub 实现（C1）
//
// 双重用途：
//  1) 非 Windows 平台的降级实现（UTF-8 内置编解码、无 mmap 时按窗口 fstream
//     预读到堆缓冲——每窗口 64MB，内存同样有界；GBK 不支持返回 false）。
//  2) 接口完备性编译验证：Windows 下定义 TEXTSEARCH_PORT_STUB 与 port_win.cpp
//     互斥编译，证明 core 其余代码只依赖 port 头文件、零 Win32 残留
//     （移植层验收项：只验编译面，GBK 用例在该配置下预期失败）。
#if !defined(_WIN32) || defined(TEXTSEARCH_PORT_STUB)

#include "core/port/port_codec.h"
#include "core/port/port_fs.h"
#include "core/port/port_mmap.h"

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <vector>

namespace port {

// ---- codec：内置 UTF-8 编解码（纯 C++，无平台依赖）----

static void append_utf8_replacement(std::wstring& out) {
    out.push_back(L'\uFFFD');
}

// 返回 [-1, 0x10FFFF]；-1 = 非法序列；前向指针由调用方推进
static int32_t decode_utf8_unit(const unsigned char* p, size_t len, size_t& adv) {
    unsigned char b = p[0];
    if (b < 0x80) { adv = 1; return b; }
    int32_t cp = 0;
    size_t need = 0;
    if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; need = 2; }
    else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; need = 3; }
    else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; need = 4; }
    else return -1;
    if (len < need) return -1;
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    // 过长编码 / 代理区 / 超 U+10FFFF 均非法
    static const int32_t kMinPerLen[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMinPerLen[need] || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return -1;
    }
    adv = need;
    return cp;
}

static void append_code_point(std::wstring& out, uint32_t cp) {
    if (sizeof(wchar_t) == 2) {
        // UTF-16：代理对
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    } else {
        out.push_back(static_cast<wchar_t>(cp));   // UCS-4 wchar_t
    }
}

bool mb_to_wide(const char* data, size_t len, Codec codec, bool strict,
                std::wstring& out) {
    out.clear();
    if (len == 0) return true;
    if (codec == Codec::Gbk) return false;   // stub 不带 GBK 表
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    size_t i = 0;
    while (i < len) {
        size_t adv = 0;
        int32_t cp = decode_utf8_unit(p + i, len - i, adv);
        if (cp < 0) {
            if (strict) return false;
            append_utf8_replacement(out);
            ++i;   // 跳过非法首字节，尽量向后恢复
            continue;
        }
        append_code_point(out, static_cast<uint32_t>(cp));
        i += adv;
    }
    return true;
}

bool wide_to_utf8(const wchar_t* data, size_t len, std::string& out) {
    out.clear();
    if (len == 0) return true;
    auto push = [&](uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };
    size_t i = 0;
    while (i < len) {
        wchar_t c = data[i];
        if (sizeof(wchar_t) == 2 && c >= 0xD800 && c <= 0xDBFF &&
            i + 1 < len && data[i + 1] >= 0xDC00 && data[i + 1] <= 0xDFFF) {
            uint32_t cp = 0x10000 +
                          ((static_cast<uint32_t>(c) - 0xD800) << 10) +
                          (static_cast<uint32_t>(data[i + 1]) - 0xDC00);
            push(cp);
            i += 2;
        } else {
            push(static_cast<uint32_t>(c));
            ++i;
        }
    }
    return true;
}

// ---- fs ----

std::wstring to_long_path(const std::wstring& path) { return path; }

bool is_reparse_point(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_symlink(p, ec);
}

bool set_hidden(const std::filesystem::path&) { return true; }   // no-op

std::filesystem::path app_data_dir() {
    // XDG 约定；无 XDG_DATA_HOME 时用 ~/.local/share
    std::string base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) base = xdg;
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        if (home) base = std::string(home) + "/.local/share";
    }
    std::filesystem::path dir =
        base.empty() ? std::filesystem::current_path()
                     : std::filesystem::path(base);
    std::filesystem::path app_dir = dir / "textsearch";
    std::error_code ec;
    std::filesystem::create_directories(app_dir, ec);
    return app_dir;
}

// ---- mmap：无内存映射时的窗口预读（每窗口一块堆缓冲，内存同样有界）----

struct SlidingFileMap::Impl {
    std::ifstream ifs;
    uint64_t fsize = 0;
    std::vector<unsigned char> buf;
};

SlidingFileMap::SlidingFileMap() : impl_(std::make_unique<Impl>()) {}

SlidingFileMap::~SlidingFileMap() { close(); }

bool SlidingFileMap::open(const std::filesystem::path& path) {
    close();
    impl_->ifs.open(path, std::ios::binary);
    if (!impl_->ifs) return false;
    impl_->ifs.seekg(0, std::ios::end);
    auto end = impl_->ifs.tellg();
    if (end < 0) { close(); return false; }
    impl_->fsize = static_cast<uint64_t>(end);
    if (impl_->fsize == 0) { close(); return false; }
    return true;
}

bool SlidingFileMap::is_open() const { return impl_->ifs.is_open(); }

uint64_t SlidingFileMap::size() const { return impl_->fsize; }

const unsigned char* SlidingFileMap::map(uint64_t offset, size_t len) {
    if (!impl_->ifs.is_open()) return nullptr;
    if (offset >= impl_->fsize || len == 0) return nullptr;
    if (offset + len > impl_->fsize)
        len = static_cast<size_t>(impl_->fsize - offset);
    impl_->buf.resize(len);
    impl_->ifs.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    impl_->ifs.read(reinterpret_cast<char*>(impl_->buf.data()),
                    static_cast<std::streamsize>(len));
    if (impl_->ifs.gcount() != static_cast<std::streamsize>(len)) return nullptr;
    return impl_->buf.data();
}

void SlidingFileMap::close() {
    if (!impl_) return;
    if (impl_->ifs.is_open()) impl_->ifs.close();
    impl_->buf.clear();
    impl_->buf.shrink_to_fit();
    impl_->fsize = 0;
}

}  // namespace port

#endif  // !_WIN32 || TEXTSEARCH_PORT_STUB
