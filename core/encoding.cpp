// core/encoding.cpp — 编码自动识别实现
// 代码页转换经 core/port/port_codec（C1），本文件无平台头依赖。
#include "core/encoding.h"
#include "core/port/port_codec.h"
#include "core/util.h"

// 计算尾部「不完整的多字节序列」长度（0~3）
//
// 背景：detect_encoding 常被喂入文件头部采样（is_textual 只取前 8192 字节），
// 8192 字节边界极可能正好切断一个多字节汉字，导致严格 UTF-8 解码失败。
// 例：某会话文件末 4 字节 be 8e e7 9b，e7 是三字节首字节却只剩 2 字节。
static int utf8_tail_incomplete(const std::vector<unsigned char>& bytes) {
    size_t n = bytes.size();
    if (n == 0) return 0;
    // 从末尾向前回溯最多 3 字节，找 UTF-8 多字节首字节
    for (size_t k = 1; k <= 3 && k <= n; ++k) {
        unsigned char b = bytes[n - k];
        int need;
        if ((b & 0xE0) == 0xC0)      need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else continue;  // 续字节或 ASCII → 继续向前找
        // 该首字节需要 need 字节，但尾部只剩 k 字节
        return (static_cast<int>(k) < need) ? static_cast<int>(k) : 0;
    }
    return 0;
}

static bool try_utf8_decode_impl(const unsigned char* data, int len) {
    if (len <= 0) return true;  // 空文件视为 UTF-8
    std::wstring wstr;
    if (!port::mb_to_wide(reinterpret_cast<const char*>(data),
                          static_cast<size_t>(len), port::Codec::Utf8,
                          /*strict=*/true, wstr)) {
        return false;
    }
    // 只拒绝 null 字符：可避免 UTF-16 LE 无 BOM 文件被误判为 UTF-8
    // （0x00 在 UTF-8 中是合法的 U+0000）
    //
    // 注意（2026-08-31 修复的 P0）：旧实现额外拒绝 U+FFFD，导致内容里
    // 本来含替换符 '�' 的合法 UTF-8 文件被判为非 UTF-8 → Unknown →
    // decode_bytes 返回空串 → 整个文件静默消失。实测一个 1.83MB 会话文件
    // 仅因含 3 个 U+FFFD 就被完整丢弃。判定编码只应看「字节序列是否合法」，
    // 与内容语义无关；严格模式已保证非法序列直接失败。
    return wstr.find(L'\0') == std::wstring::npos;
}

// 严格 UTF-8 解码尝试：字节序列合法且不含 null 字符 → true
// 完整解码失败时，若尾部存在不完整的多字节序列，截掉后重试一次（应对头部采样截断）
static bool try_utf8_decode(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return true;
    if (try_utf8_decode_impl(bytes.data(), static_cast<int>(bytes.size()))) {
        return true;
    }
    int trim = utf8_tail_incomplete(bytes);
    if (trim > 0) {
        return try_utf8_decode_impl(bytes.data(),
                                    static_cast<int>(bytes.size()) - trim);
    }
    return false;
}

static bool try_gbk_decode_impl(const unsigned char* data, int len) {
    if (len <= 0) return true;
    std::wstring wstr;
    if (!port::mb_to_wide(reinterpret_cast<const char*>(data),
                          static_cast<size_t>(len), port::Codec::Gbk,
                          /*strict=*/true, wstr)) {
        return false;
    }
    // 解码后检查是否含 null 字符（U+0000），避免 UTF-16 LE 无 BOM 被误判为 GBK
    return wstr.find(L'\0') == std::wstring::npos;
}

// GBK(CP 936) 解码尝试：成功且不含 null 字符 → true
// 失败时若末字节疑似被截断的 GBK 首字节（0x81~0xFE），截掉后重试一次
static bool try_gbk_decode(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) return true;
    if (try_gbk_decode_impl(bytes.data(), static_cast<int>(bytes.size()))) {
        return true;
    }
    if (!bytes.empty() && bytes.back() >= 0x81) {
        return try_gbk_decode_impl(bytes.data(),
                                   static_cast<int>(bytes.size()) - 1);
    }
    return false;
}

// 无 BOM 的 UTF-16 LE 疑似检测：偶数长度且奇数位（0-indexed 1,3,5...）全 0x00
static bool looks_like_utf16le_nobom(const std::vector<unsigned char>& bytes) {
    if (bytes.size() < 2 || bytes.size() % 2 != 0) return false;
    bool has_nonzero_even = false;
    for (size_t i = 1; i < bytes.size(); i += 2) {
        if (bytes[i] != 0x00) return false;
    }
    for (size_t i = 0; i < bytes.size(); i += 2) {
        if (bytes[i] != 0x00) { has_nonzero_even = true; break; }
    }
    return has_nonzero_even;
}

Encoding detect_encoding(const std::vector<unsigned char>& bytes) {
    // 步骤 1: UTF-8 BOM (EF BB BF)
    if (bytes.size() >= 3 &&
        bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return Encoding::Utf8;
    }
    // 步骤 1.5（S5.5）: UTF-32 BOM 特判。FF FE 00 00 以 UTF-16 LE BOM
    // (FF FE) 开头，若不在前面拦截会被误判为 UTF-16 LE 并按 16 位解码出
    // 乱码；本程序不支持 UTF-32 → 显式归 Unknown（宁可跳过不乱解）。
    if (bytes.size() >= 4 &&
        bytes[0] == 0xFF && bytes[1] == 0xFE &&
        bytes[2] == 0x00 && bytes[3] == 0x00) {
        return Encoding::Unknown;   // UTF-32 LE BOM
    }
    if (bytes.size() >= 4 &&
        bytes[0] == 0x00 && bytes[1] == 0x00 &&
        bytes[2] == 0xFE && bytes[3] == 0xFF) {
        return Encoding::Unknown;   // UTF-32 BE BOM
    }
    // 步骤 2: UTF-16 LE BOM (FF FE)
    if (bytes.size() >= 2 &&
        bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return Encoding::Utf16Le;
    }
    // 步骤 3: UTF-16 BE BOM (FE FF)
    if (bytes.size() >= 2 &&
        bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return Encoding::Utf16Be;
    }
    // 步骤 4: 严格 UTF-8 解码
    if (try_utf8_decode(bytes)) {
        return Encoding::Utf8;
    }
    // 步骤 5: GBK (CP 936) 解码
    if (try_gbk_decode(bytes)) {
        return Encoding::Gbk;
    }
    // 步骤 6: 无 BOM 但 "0x00 间隔出现" → 疑似 UTF-16 LE
    if (looks_like_utf16le_nobom(bytes)) {
        return Encoding::Utf16Le;
    }
    // 步骤 7: 都失败 → Unknown
    return Encoding::Unknown;
}

std::wstring decode_bytes(const std::vector<unsigned char>& bytes, Encoding enc) {
    switch (enc) {
        case Encoding::Utf8: {
            // 跳过 BOM（如有）
            const unsigned char* data = bytes.data();
            size_t len = bytes.size();
            if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
                data += 3;
                len -= 3;
            }
            if (len == 0) return std::wstring();
            std::wstring wstr;
            if (!port::mb_to_wide(reinterpret_cast<const char*>(data), len,
                                  port::Codec::Utf8, /*strict=*/false, wstr)) {
                return std::wstring();
            }
            return wstr;
        }
        case Encoding::Gbk: {
            if (bytes.empty()) return std::wstring();
            std::wstring wstr;
            if (!port::mb_to_wide(reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size(), port::Codec::Gbk,
                                  /*strict=*/false, wstr)) {
                return std::wstring();
            }
            return wstr;
        }
        case Encoding::Utf16Le: {
            // 跳过 BOM（如有）
            size_t offset = 0;
            if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
                offset = 2;
            }
            if (bytes.size() <= offset) return std::wstring();
            // Windows wchar_t = UTF-16LE，可直接 reinterpret
            const wchar_t* p = reinterpret_cast<const wchar_t*>(bytes.data() + offset);
            size_t count = (bytes.size() - offset) / sizeof(wchar_t);
            return std::wstring(p, count);
        }
        case Encoding::Utf16Be: {
            // 跳过 BOM（如有）
            size_t offset = 0;
            if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
                offset = 2;
            }
            if (bytes.size() <= offset) return std::wstring();
            std::wstring result;
            for (size_t i = offset; i + 1 < bytes.size(); i += 2) {
                // BE → LE：交换高低字节
                wchar_t ch = (static_cast<wchar_t>(bytes[i]) << 8) | bytes[i + 1];
                result.push_back(ch);
            }
            return result;
        }
        default: {
            // Unknown：无法判定编码。
            //
            // 旧实现在此返回空串，后果是「整个文件静默消失」——实时扫描和
            // 索引都搜不到，界面无任何提示（2026-08-31 会话检索实验发现的 P0）。
            //
            // 改为 UTF-8 宽松解码：非法字节替换为 U+FFFD，其余内容保持可读。
            // 走到这里的文件已通过 is_textual 二进制探测，宽松解码不会引入噪声。
            // 「内容部分乱码」远优于「文件整个消失」——后者是零漏报目标的直接违反。
            if (bytes.empty()) return std::wstring();
            const unsigned char* data = bytes.data();
            size_t len = bytes.size();
            if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
                data += 3;
                len -= 3;
            }
            if (len == 0) return std::wstring();
            std::wstring wstr;
            if (!port::mb_to_wide(reinterpret_cast<const char*>(data), len,
                                  port::Codec::Utf8, /*strict=*/false, wstr)) {
                return std::wstring();
            }
            return wstr;
        }
    }
}

std::wstring read_text(const std::filesystem::path& path) {
    // R6: 一律 binary 模式读取
    std::vector<unsigned char> bytes = read_file_bytes(path);
    if (bytes.empty()) return std::wstring();
    Encoding enc = detect_encoding(bytes);
    return decode_bytes(bytes, enc);
}
