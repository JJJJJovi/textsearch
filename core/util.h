// core/util.h — 通用工具函数声明
#pragma once

#include <filesystem>
#include <string>
#include <vector>

// ---- 编码转换 ----

// UTF-8 std::string → std::wstring（UTF-16LE）
std::wstring utf8_to_wstring(const std::string& str);

// std::wstring → UTF-8 std::string
std::string wstring_to_utf8(const std::wstring& wstr);

// ---- 字符串工具 ----

// 宽字符串转小写（使用 towlower）
std::wstring to_lower_wstring(const std::wstring& s);

// 判断字符是否为单词字符 [A-Za-z0-9_]
bool is_word_char(wchar_t c);

// ---- 路径工具 ----

// 获取 %LOCALAPPDATA%/TextSearch 目录（不存在则创建）
std::filesystem::path get_app_data_dir();

// 长路径加 \\?\ 前缀（R19），超过 MAX_PATH 时自动添加
std::wstring to_long_path(const std::wstring& path);

// 检查路径是否为 reparse point（符号链接 / junction，R11）
bool is_reparse_point(const std::filesystem::path& p);

// 通配符匹配（支持 * 和 ?）
bool wildcard_match(const std::wstring& pattern, const std::wstring& text);

// ---- 文件读取 ----

// 读取文件前 n 字节（二进制模式）
std::vector<unsigned char> read_head_bytes(const std::filesystem::path& path, size_t n);

// 读取文件全部字节（二进制模式）
std::vector<unsigned char> read_file_bytes(const std::filesystem::path& path);

// 文件 mtime → unix 秒（**确定性**转换）
//
// ⚠️ 不要用 `ftime - file_time_type::clock::now() + system_clock::now()`。
// 该式每次调用都采样两次 now()，两次采样的微秒级间隔会让「秒级截断」在秒边界
// 附近抖动 ±1 秒。后果：索引刚建完就被误判「文件已修改」，导致每次增量更新
// 都触发一次无谓的全量 load + 重写（大索引上代价极高），且误判方向随机难复现。
//
// 本函数改为：进程内只校准一次 epoch 偏移（static，C++11 magic static 线程安全），
// 之后纯算术，保证同一个 ftime 永远得到同一个秒值。
// 索引侧（build / update_incremental）与扫描侧（mtime_filter）**均须使用本函数**，
// 否则两侧算出的秒值可能不一致，导致「刚重建的文件仍被判为已修改」。
int64_t file_mtime_unix(const std::filesystem::file_time_type& ft);
