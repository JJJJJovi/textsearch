// core/text_detect.cpp — 文本/二进制判定实现
#include "core/text_detect.h"
#include "core/encoding.h"
#include "core/util.h"

// NUL 探测缓冲区大小
static constexpr size_t HEAD_BUF_SIZE = 8192;

bool is_textual(const std::vector<unsigned char>& head) {
    // 附录 F.2.1：先调用 detect_encoding 预检测
    // 已识别为 UTF-16 的不进 NUL 判定（UTF-16 合法含 0x00 字节）
    Encoding enc = detect_encoding(head);

    if (enc == Encoding::Utf16Le || enc == Encoding::Utf16Be) {
        // UTF-16 文件，跳过 NUL 探测
        return true;
    }

    // 对于 Utf8、Gbk、Unknown：做 NUL 探测
    // 文本文件不应含 NUL 字节（0x00），含则判定为二进制
    // 空文件视为文本
    for (unsigned char b : head) {
        if (b == 0x00) return false;
    }
    return true;
}

bool is_textual_file(const std::filesystem::path& path) {
    std::vector<unsigned char> head = read_head_bytes(path, HEAD_BUF_SIZE);
    return is_textual(head);
}
