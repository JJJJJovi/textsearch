// tests/test_encoding.cpp — 编码识别测试
#include "simpletest.h"
#include "core/encoding.h"
#include "core/util.h"

#include <filesystem>
#include <fstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static constexpr UINT CP_GBK = 936;

// 写入字节数据到临时文件
static std::filesystem::path write_bytes(const std::wstring& name,
                                          const std::vector<unsigned char>& data) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return path;
}

// 生成 GBK 编码字节
static std::vector<unsigned char> make_gbk(const std::wstring& text) {
    int len = WideCharToMultiByte(CP_GBK, 0,
        text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::vector<unsigned char> buf(static_cast<size_t>(len));
    WideCharToMultiByte(CP_GBK, 0,
        text.data(), static_cast<int>(text.size()),
        reinterpret_cast<char*>(buf.data()), len, nullptr, nullptr);
    return buf;
}

// 生成 UTF-8 编码字节
static std::vector<unsigned char> make_utf8(const std::wstring& text) {
    std::string s = wstring_to_utf8(text);
    return std::vector<unsigned char>(s.begin(), s.end());
}

// ---- detect_encoding 测试 ----

TEST_CASE(enc_utf8_bom) {
    std::vector<unsigned char> bytes = {0xEF, 0xBB, 0xBF, 'H', 'i'};
    REQUIRE(detect_encoding(bytes) == Encoding::Utf8);
}

TEST_CASE(enc_utf16le_bom) {
    std::vector<unsigned char> bytes = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
    REQUIRE(detect_encoding(bytes) == Encoding::Utf16Le);
}

TEST_CASE(enc_utf16be_bom) {
    std::vector<unsigned char> bytes = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};
    REQUIRE(detect_encoding(bytes) == Encoding::Utf16Be);
}

TEST_CASE(enc_utf32_bom_unknown) {
    // S5.5：UTF-32 BOM 特判归 Unknown（旧实现把 FF FE 00 00 误判 UTF-16 LE）
    std::vector<unsigned char> le = {0xFF, 0xFE, 0x00, 0x00, 'H', 0, 0, 0};
    REQUIRE(detect_encoding(le) == Encoding::Unknown);
    std::vector<unsigned char> be = {0x00, 0x00, 0xFE, 0xFF, 0, 0, 'H', 0};
    REQUIRE(detect_encoding(be) == Encoding::Unknown);
    // 真 UTF-16 LE BOM（第 3/4 字节非 00 00）不受影响
    std::vector<unsigned char> u16 = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
    REQUIRE(detect_encoding(u16) == Encoding::Utf16Le);
}

TEST_CASE(enc_utf8_no_bom_chinese) {
    // "中文" in UTF-8: E4 B8 AD E6 96 87
    auto bytes = make_utf8(L"中文");
    REQUIRE(detect_encoding(bytes) == Encoding::Utf8);
}

TEST_CASE(enc_gbk_no_bom_chinese) {
    // "中文" in GBK: D6 D0 CE C4
    auto bytes = make_gbk(L"中文");
    REQUIRE(detect_encoding(bytes) == Encoding::Gbk);
}

TEST_CASE(enc_utf16le_no_bom) {
    // "Hello" in UTF-16 LE without BOM
    std::vector<unsigned char> bytes = {
        'H', 0x00, 'e', 0x00, 'l', 0x00, 'l', 0x00, 'o', 0x00
    };
    REQUIRE(detect_encoding(bytes) == Encoding::Utf16Le);
}

TEST_CASE(enc_empty) {
    std::vector<unsigned char> bytes;
    REQUIRE(detect_encoding(bytes) == Encoding::Utf8); // 空文件视为 UTF-8
}

TEST_CASE(enc_pure_ascii) {
    std::vector<unsigned char> bytes = {'H', 'e', 'l', 'l', 'o'};
    REQUIRE(detect_encoding(bytes) == Encoding::Utf8); // ASCII 是合法 UTF-8
}

// ---- read_text 测试 ----

TEST_CASE(enc_read_utf8_file) {
    auto bytes = make_utf8(L"中文测试abc");
    auto path = write_bytes(L"ts_test_utf8.txt", bytes);
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, L"中文测试abc");
    std::filesystem::remove(path);
}

TEST_CASE(enc_read_gbk_file) {
    auto bytes = make_gbk(L"中文测试abc");
    auto path = write_bytes(L"ts_test_gbk.txt", bytes);
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, L"中文测试abc");
    std::filesystem::remove(path);
}

TEST_CASE(enc_read_utf8_bom_file) {
    std::vector<unsigned char> bytes = {0xEF, 0xBB, 0xBF};
    auto utf8 = make_utf8(L"带BOM的UTF8");
    bytes.insert(bytes.end(), utf8.begin(), utf8.end());
    auto path = write_bytes(L"ts_test_utf8bom.txt", bytes);
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, L"带BOM的UTF8");
    std::filesystem::remove(path);
}

TEST_CASE(enc_read_utf16le_bom_file) {
    std::vector<unsigned char> bytes = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
    auto path = write_bytes(L"ts_test_utf16le.txt", bytes);
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, L"Hi");
    std::filesystem::remove(path);
}

TEST_CASE(enc_read_utf16be_bom_file) {
    std::vector<unsigned char> bytes = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};
    auto path = write_bytes(L"ts_test_utf16be.txt", bytes);
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, L"Hi");
    std::filesystem::remove(path);
}

// ── 2026-08-31 会话检索实验发现的 P0 回归用例 ──────────────────────────
// 背景：一个 1.83MB 的真实会话文件，仅因内容含 3 个 U+FFFD 替换符，
// 就被 detect_encoding 判为 Unknown → decode_bytes 返回空串 →
// 整个文件在实时扫描和索引中静默消失，界面无任何提示。
// 以下用例锁死修复后的行为，防止回归。

TEST_CASE(enc_utf8_with_replacement_char) {
    // 合法 UTF-8，但内容含 U+FFFD 替换符 → 必须仍判定为 Utf8
    // （旧实现因拒绝 U+FFFD 而误判为非 UTF-8）
    std::wstring text = L"开机" L"\uFFFD\uFFFD\uFFFD" L"后把这几个文件放";
    auto bytes = make_utf8(text);
    REQUIRE_EQ(detect_encoding(bytes), Encoding::Utf8);
}

TEST_CASE(enc_read_utf8_file_with_replacement_char) {
    // 端到端：含 U+FFFD 的文件，read_text 不得返回空串（旧行为 = 文件消失）
    std::wstring text = L"前缀内容" L"\uFFFD" L"后缀内容";
    auto path = write_bytes(L"ts_test_utf8_replacement.txt", make_utf8(text));
    std::wstring result = read_text(path);
    REQUIRE_EQ(result, text);
    std::filesystem::remove(path);
}

TEST_CASE(enc_head_sampling_truncated_multibyte) {
    // 头部采样（is_textual 只取前 8192 字节）常在多字节汉字中间切断。
    // 构造：让第 8192 字节正好落在一个三字节汉字的中间。
    std::wstring pad(8190, L'a');
    auto bytes = make_utf8(pad + L"中文");  // 8190 + 6 = 8196 字节
    std::vector<unsigned char> head(bytes.begin(), bytes.begin() + 8192);
    REQUIRE_EQ(head.size(), (size_t)8192);
    // 修复前：严格解码失败 → GBK 也失败 → Unknown（导致文件被丢弃）
    REQUIRE_EQ(detect_encoding(head), Encoding::Utf8);
}

TEST_CASE(enc_unknown_fallback_lossy_utf8) {
    // 编码无法判定（Unknown）时，不得返回空串让文件消失；
    // 必须做 UTF-8 宽松解码：非法字节替换为 U+FFFD，其余内容保持可读。
    std::vector<unsigned char> bytes;
    auto head = make_utf8(L"hello timestamp world");
    bytes.insert(bytes.end(), head.begin(), head.end());
    bytes.push_back(0xC3);  // UTF-8 二字节首字节
    bytes.push_back(0x28);  // '(' 不是合法续字节 → 强制解码失败
    auto tail = make_utf8(L"尾部内容");
    bytes.insert(bytes.end(), tail.begin(), tail.end());

    auto enc = detect_encoding(bytes);
    auto out = decode_bytes(bytes, enc);
    // 关键断言：内容不为空，且损坏点前后的内容都可读
    REQUIRE(!out.empty());
    REQUIRE(out.find(L"timestamp") != std::wstring::npos);
    REQUIRE(out.find(L"尾部内容") != std::wstring::npos);
}
