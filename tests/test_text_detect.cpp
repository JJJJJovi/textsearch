// tests/test_text_detect.cpp — 文本/二进制判定测试
#include "simpletest.h"
#include "core/text_detect.h"
#include "core/encoding.h"

#include <vector>

TEST_CASE(td_ascii_text) {
    std::vector<unsigned char> head = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
    REQUIRE(is_textual(head));
}

TEST_CASE(td_empty_file) {
    std::vector<unsigned char> head;
    REQUIRE(is_textual(head)); // 空文件视为文本
}

TEST_CASE(td_binary_with_nul) {
    std::vector<unsigned char> head = {0x89, 0x50, 0x4E, 0x00, 0x0D, 0x0A};
    REQUIRE(!is_textual(head)); // 含 NUL → 二进制
}

TEST_CASE(td_utf8_chinese) {
    // "中文" in UTF-8: E4 B8 AD E6 96 87
    std::vector<unsigned char> head = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87};
    REQUIRE(is_textual(head));
}

TEST_CASE(td_utf16le_bom) {
    // UTF-16 LE BOM + "Hi"
    std::vector<unsigned char> head = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
    REQUIRE(is_textual(head));
}

TEST_CASE(td_utf16le_no_bom) {
    // "Hello" in UTF-16 LE without BOM: H=48 00, e=65 00, ...
    std::vector<unsigned char> head = {
        'H', 0x00, 'e', 0x00, 'l', 0x00, 'l', 0x00, 'o', 0x00
    };
    // 先检测编码 → Utf16Le → 不进 NUL 探测 → true
    REQUIRE(is_textual(head));
}

TEST_CASE(td_utf16le_no_bom_detected) {
    // 验证 detect_encoding 正确识别无 BOM UTF-16 LE
    std::vector<unsigned char> head = {
        'H', 0x00, 'e', 0x00, 'l', 0x00, 'l', 0x00, 'o', 0x00
    };
    REQUIRE(detect_encoding(head) == Encoding::Utf16Le);
}

TEST_CASE(td_utf16be_bom) {
    // UTF-16 BE BOM + "Hi"
    std::vector<unsigned char> head = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};
    REQUIRE(is_textual(head));
}

TEST_CASE(td_gbk_text) {
    // GBK "中文": D6 D0 CE C4
    std::vector<unsigned char> head = {0xD6, 0xD0, 0xCE, 0xC4};
    REQUIRE(is_textual(head));
}
