// core/extension_manager.h — 常用扩展名管理接口
//
// 术语约定（重要）：
// - **UI 与配置里一律称「常用扩展名」**。"白名单"易被误解为屏蔽名单/安全白名单。
//   配置文件中的 key 相应为 `[Extensions] common`。
// - **代码内部标识符沿用 whitelist**（load_whitelist / save_whitelist / default_whitelist），
//   英文 allow-list 语义无歧义，且避免大范围重命名；二者为同一份数据。
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

// 常用扩展名管理器：加载 / 保存 / 添加（抽样验证）/ 删除
struct ExtensionManager {
    // 读取常用扩展名（textsearch.ini 的 [Extensions] common）；
    // 兼容回退到旧 key whitelist、旧 whitelist.json；都没有则返回内置默认列表
    std::set<std::wstring> load_whitelist();

    // 添加扩展名（抽样验证：在 sample_dir 递归找该扩展名文件，
    // 取前 sample_n 个，逐个 is_textual + read_text 解码成功才算通过）
    bool add_extension(const std::wstring& ext,
                       const std::filesystem::path& sample_dir,
                       int sample_n = 3);

    // 删除扩展名
    void remove_extension(const std::wstring& ext);

    // 保存常用扩展名到 textsearch.ini 的 [Extensions] common
    // （会清掉可能并存的旧 key whitelist，避免两处配置歧义）
    bool save_whitelist(const std::set<std::wstring>& set);
};

// 扩展名集合解析（Q3=A）：
// 以"用户勾选(dir_checked)"为最终集合；仅当 dir_checked 为空时回退常用扩展名全集。
// 再减去 黑名单 与 通配排除。常用扩展名不再无条件并入（否则用户取消勾选的项仍会被搜到）。
std::set<std::wstring> resolve_ext_set(
    const std::set<std::wstring>& whitelist,
    const std::set<std::wstring>& dir_checked,
    const std::vector<std::wstring>& blacklist,
    const std::vector<std::wstring>& wildcards);

// 内置预置文本类扩展名列表（仅在配置文件缺失时兜底，用户可在 UI 中修改）
std::set<std::wstring> default_whitelist();

// 配置文件路径：%LOCALAPPDATA%/TextSearch/textsearch.ini
// （函数名 whitelist_path 为历史遗留，实际返回的是统一配置文件路径）
std::filesystem::path whitelist_path();

// 添加扩展名的失败原因（供 UI 提示）
std::wstring last_add_error();
