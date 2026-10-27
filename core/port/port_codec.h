// core/port/port_codec.h — 编码转换移植接口（C1）
//
// core 层唯二的编码转换需求：多字节(UTF-8/GBK) ↔ wchar_t。
// Windows 实现 = Win32 API（MultiByteToWideChar / WideCharToMultiByte）；
// 其他平台由移植实现提供（stub：UTF-8 为内置纯 C++ 实现，GBK 不支持返回 false）。
// 语义约定（与原 Win32 调用逐一对齐，行为不变）：
//   strict=true  → 任何非法序列整体失败返回 false（Win32 MB_ERR_INVALID_CHARS）
//   strict=false → 非法序列替换为 U+FFFD 尽力转换（Win32 flags=0）
#pragma once

#include <cstddef>
#include <string>

namespace port {

enum class Codec { Utf8, Gbk };

// 多字节 → UTF-16（wchar_t 串）。成功返回 true 并写 out；失败返回 false。
bool mb_to_wide(const char* data, size_t len, Codec codec, bool strict,
                std::wstring& out);

// wchar_t 串 → UTF-8。成功返回 true 并写 out。
bool wide_to_utf8(const wchar_t* data, size_t len, std::string& out);

}  // namespace port
