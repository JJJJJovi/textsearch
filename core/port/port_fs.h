// core/port/port_fs.h — 文件系统差异移植接口（C1）
//
// 收口 core 此前散落在 util.cpp / index_store.cpp 的 Win32 文件系统调用：
//   长路径 \\?\ 前缀、reparse point/junction 探测、目录隐藏属性、应用数据目录。
// Windows 之外：长路径原样返回、reparse 退化为 is_symlink、隐藏为 no-op、
// 应用数据目录走 XDG 约定。
#pragma once

#include <filesystem>
#include <string>

namespace port {

// 超长路径加 \\?\ 前缀（Windows MAX_PATH 限制；其他平台原样返回）。
std::wstring to_long_path(const std::wstring& path);

// 是否 reparse point（符号链接 / junction）。Windows 之外退化为 is_symlink
// （junction 是 NTFS 概念，其他平台不存在）。
bool is_reparse_point(const std::filesystem::path& p);

// 把「目录自身」标记为隐藏（仅 Windows FILE_ATTRIBUTE_HIDDEN；其他平台 no-op）。
// 失败不上抛（FAT/exFAT/只读介质都可能失败），返回是否成功。
bool set_hidden(const std::filesystem::path& dir);

// 应用数据目录（Windows：%LOCALAPPDATA%/TextSearch；其他平台：
// $XDG_DATA_HOME/textsearch 或 ~/.local/share/textsearch）。不存在则创建。
std::filesystem::path app_data_dir();

}  // namespace port
