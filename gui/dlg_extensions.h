// gui/dlg_extensions.h — 扩展名收集对话框（T5 5.4b）
// 递归收集目录下所有扩展名（去重）→ 预勾选已知文本类（常用扩展名∩收集）
// → 用户勾选确认 → 返回勾选集合，加入待搜对象。
// 另支持手动添加目录中不存在的扩展名（输入框 + 添加按钮，如 .xyz），
// 加入即默认勾选；为会话内临时添加，不写回常用扩展名。
// 黑名单/通配由主窗口在 resolve_ext_set 阶段统一处理。
#pragma once

#include <filesystem>
#include <set>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dlg_extensions {

// 收集目录下所有扩展名（去重、小写、含前导点）。递归遍历，跳过 reparse point。
// - root:      根目录
// - recursive: 是否递归子目录
std::set<std::wstring> collect_all_extensions(const std::filesystem::path& root,
                                              bool recursive);

// 显示模态扩展名收集对话框。
// - parent:    父窗口
// - hInst:     实例句柄
// - root:      要收集的目录
// - whitelist: 已知常用扩展名（用于预勾选）
// - checked:   [out] 用户确认后勾选的扩展名集合
// 返回值：true=用户确认（checked 已填充）；false=用户取消。
bool show(HWND parent, HINSTANCE hInst,
          const std::filesystem::path& root,
          const std::set<std::wstring>& whitelist,
          std::set<std::wstring>& checked);

} // namespace dlg_extensions
