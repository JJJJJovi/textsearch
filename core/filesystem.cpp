// core/filesystem.cpp — 文件遍历实现
#include "core/filesystem.h"
#include "core/util.h"

#include <cwctype>
#include <system_error>

// 内部：将扩展名转为小写
static std::wstring ext_to_lower(const std::filesystem::path& p) {
    std::wstring ext = p.extension().wstring();
    std::wstring result;
    result.reserve(ext.size());
    for (wchar_t c : ext) {
        result.push_back(static_cast<wchar_t>(towlower(static_cast<wint_t>(c))));
    }
    return result;
}

std::vector<std::filesystem::path>
enumerate_files(const std::filesystem::path& root,
                const std::set<std::wstring>& ext_set,
                bool recursive) {
    std::vector<std::filesystem::path> result;

    if (ext_set.empty()) return result;

    std::error_code ec;

    if (recursive) {
        // 递归遍历，skip_permission_denied 跳过无权限目录
        auto opts = std::filesystem::directory_options::skip_permission_denied;
        auto iter = std::filesystem::recursive_directory_iterator(root, opts, ec);
        if (ec) return result;

        auto end = std::filesystem::recursive_directory_iterator();
        for (; iter != end; ++iter, ec.clear()) {
            const auto& entry = *iter;

            // 跳过符号链接 / junction（R11）
            if (is_reparse_point(entry.path())) {
                if (entry.is_directory(ec)) {
                    iter.disable_recursion_pending();
                }
                continue;
            }

            // 只处理常规文件
            if (!entry.is_regular_file(ec)) continue;

            // 扩展名匹配（统一转小写比较）
            std::wstring ext = ext_to_lower(entry.path());
            if (ext_set.count(ext) > 0) {
                result.push_back(entry.path());
            }
        }
    } else {
        // 非递归遍历
        auto iter = std::filesystem::directory_iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec) return result;

        auto end = std::filesystem::directory_iterator();
        for (; iter != end; ++iter, ec.clear()) {
            const auto& entry = *iter;

            // 跳过符号链接 / junction
            if (is_reparse_point(entry.path())) continue;

            if (!entry.is_regular_file(ec)) continue;

            std::wstring ext = ext_to_lower(entry.path());
            if (ext_set.count(ext) > 0) {
                result.push_back(entry.path());
            }
        }
    }

    return result;
}
