// core/encoding.h — 编码自动识别接口
#pragma once

#include "core/types.h"

#include <filesystem>
#include <string>
#include <vector>

// 编码自动识别（附录 F.2.1 七步决策树）
// 1. UTF-8 BOM (EF BB BF) → Utf8
// 2. UTF-16 LE BOM (FF FE) → Utf16Le
// 3. UTF-16 BE BOM (FE FF) → Utf16Be
// 4. 严格 UTF-8 解码成功 → Utf8
// 5. GBK(CP 936) 解码成功 → Gbk
// 6. 无 BOM 但 "0x00 间隔出现" → 疑似 UTF-16 LE
// 7. 都失败 → Unknown
Encoding detect_encoding(const std::vector<unsigned char>& bytes);

// 二进制读取文件 → detect_encoding → 解码为 wstring
// 禁止文本模式读文件（R6）：一律 binary 模式，自行按 \n 切行
std::wstring read_text(const std::filesystem::path& path);

// 将原始字节按指定编码解码为 wstring
std::wstring decode_bytes(const std::vector<unsigned char>& bytes, Encoding enc);
