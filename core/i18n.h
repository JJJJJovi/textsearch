// core/i18n.h — 界面多语言（零依赖内嵌翻译表，C1 同气质）
//
// 设计（2026-10-27，随 GitHub 开源需求引入）：
//   - 中文为基线语言，英文对照；条目缺失时回退中文，再退回 id 本身。
//   - 语言三选一：跟随系统 / 简体中文 / English；运行中切换 → 重启生效
//     （Win32 全控件动态换语言需重建整层 UI，收益不成比例）。
//   - tr(id) 取静态文案；trf(id, a, b, c) 对模板中的 {1}{2}{3} 做占位替换。
//   - 线程安全：set_language 仅在 main 建窗前调用一次，之后只读。
#pragma once

#include <string>

namespace i18n {

enum class Lang { System, ZhCn, EnUs };

// 由 ini 字符串（"system"/"zh"/"en"，空 = system）设置；main 建窗前调用。
void set_language_from(const std::wstring& cfg);
// 实际生效语言（System 已按 GetUserDefaultUILanguage 解析）。
Lang language();

// 取文案： zh 基线 / en 对照 / 缺失回退中文 / 无条目回退 id。
const wchar_t* tr(const char* id);

// 模板占位替换：{1}{2}{3} 依次替换为 a/b/c（空串的占位保持原样不替换）。
std::wstring trf(const char* id, const std::wstring& a = std::wstring(),
                 const std::wstring& b = std::wstring(),
                 const std::wstring& c = std::wstring());

}  // namespace i18n
