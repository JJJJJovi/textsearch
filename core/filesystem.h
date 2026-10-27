// core/filesystem.h — 文件遍历接口
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

// 递归/非递归遍历目录，返回扩展名匹配的文件列表
// - root:       根目录
// - ext_set:    已解析的最终扩展名集合（小写，含前导点，如 L".py"）
// - recursive:  是否递归子目录
// 跳过符号链接 / junction（R11），无权限目录静默跳过
std::vector<std::filesystem::path>
enumerate_files(const std::filesystem::path& root,
                const std::set<std::wstring>& ext_set,
                bool recursive);
