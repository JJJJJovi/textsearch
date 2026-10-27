// core/util.cpp — 通用工具函数实现
// 平台差异（编码转换 / 长路径 / reparse 探测 / 应用数据目录）已收口到
// core/port/（C1），本文件不再包含任何 windows.h。
#include "core/util.h"
#include "core/port/port_codec.h"
#include "core/port/port_fs.h"

#include <chrono>
#include <cwctype>
#include <fstream>

// ---- 编码转换 ----

std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) return std::wstring();
    std::wstring result;
    if (!port::mb_to_wide(str.data(), str.size(), port::Codec::Utf8,
                          /*strict=*/false, result)) {
        return std::wstring();
    }
    return result;
}

std::string wstring_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    std::string result;
    if (!port::wide_to_utf8(wstr.data(), wstr.size(), result)) {
        return std::string();
    }
    return result;
}

// ---- 字符串工具 ----

std::wstring to_lower_wstring(const std::wstring& s) {
    std::wstring result;
    result.reserve(s.size());
    for (wchar_t c : s) {
        result.push_back(static_cast<wchar_t>(towlower(static_cast<wint_t>(c))));
    }
    return result;
}

bool is_word_char(wchar_t c) {
    return (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') ||
           c == L'_';
}

// ---- 路径工具 ----

std::filesystem::path get_app_data_dir() {
    return port::app_data_dir();
}

std::wstring to_long_path(const std::wstring& path) {
    return port::to_long_path(path);
}

bool is_reparse_point(const std::filesystem::path& p) {
    return port::is_reparse_point(p);
}

bool wildcard_match(const std::wstring& pattern, const std::wstring& text) {
    // 经典通配符匹配：* 匹配任意字符序列，? 匹配单个字符
    size_t pi = 0, ti = 0;
    size_t star = std::wstring::npos;
    size_t match = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() &&
            (pattern[pi] == text[ti] || pattern[pi] == L'?')) {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == L'*') {
            star = pi;
            match = ti;
            ++pi;
        } else if (star != std::wstring::npos) {
            pi = star + 1;
            ++match;
            ti = match;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == L'*') {
        ++pi;
    }
    return pi == pattern.size();
}

// ---- 文件读取 ----

std::vector<unsigned char> read_head_bytes(const std::filesystem::path& path, size_t n) {
    std::vector<unsigned char> buf;
    // 获取文件大小，决定读取量
    std::error_code ec;
    auto fsize = std::filesystem::file_size(path, ec);
    if (ec) return buf;
    size_t to_read = static_cast<size_t>(fsize);
    if (to_read > n) to_read = n;

    buf.resize(to_read);
    if (to_read == 0) return buf;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return buf;
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(to_read));
    buf.resize(static_cast<size_t>(ifs.gcount()));
    return buf;
}

std::vector<unsigned char> read_file_bytes(const std::filesystem::path& path) {
    std::vector<unsigned char> buf;
    std::error_code ec;
    auto fsize = std::filesystem::file_size(path, ec);
    if (ec) return buf;

    buf.resize(static_cast<size_t>(fsize));
    if (fsize == 0) return buf;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return buf;
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(fsize));
    buf.resize(static_cast<size_t>(ifs.gcount()));
    return buf;
}

int64_t file_mtime_unix(const std::filesystem::file_time_type& ft) {
    using namespace std::chrono;
    // 进程内只校准一次：file_time clock epoch 与 system_clock epoch 的偏移。
    // magic static 保证并发调用也只初始化一次（build 的并行 worker 会并发调用本函数）。
    static const seconds epoch_offset = [] {
        auto fnow = std::filesystem::file_time_type::clock::now();
        auto snow = system_clock::now();
        return duration_cast<seconds>(snow.time_since_epoch() -
                                      fnow.time_since_epoch());
    }();
    return duration_cast<seconds>(ft.time_since_epoch() + epoch_offset).count();
}
