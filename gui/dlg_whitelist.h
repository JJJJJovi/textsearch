// gui/dlg_whitelist.h — 「常用扩展名」编辑对话框
//
// 让用户自行维护常见文本文件扩展名列表（原为程序内置，不客观）。
// UI 上统一称「常用扩展名」（"白名单"易被误解为屏蔽名单/安全白名单）；
// 代码内部标识符沿用 whitelist（英文无歧义），二者对应关系见本注释。
//
// 数据存于 %LOCALAPPDATA%/TextSearch/textsearch.ini 的 [Extensions] common，
// 也可直接用记事本编辑该 ini 文件。
// 作用：收集扩展名时命中此列表的项会默认勾选，并排在列表最前面。
#pragma once

#include <set>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dlg_whitelist {

// 显示模态「常用扩展名」编辑对话框。
// - parent:   父窗口
// - hInst:    实例句柄
// - current:  当前常用扩展名列表（作为编辑框初值，按字母序展示）
// - updated:  [out] 用户确认后的新列表（已规范化：小写 + 前导点）
// 返回值：true=用户确认并已写回配置文件；false=用户取消。
bool show(HWND parent, HINSTANCE hInst,
          const std::set<std::wstring>& current,
          std::set<std::wstring>& updated);

} // namespace dlg_whitelist
