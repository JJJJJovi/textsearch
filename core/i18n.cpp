// core/i18n.cpp — 界面多语言实现（零依赖内嵌表）
// 表条目：{ id, 中文(基线), English }。缺失回退中文，无条目回退 id。
// 语言解析：System → GetUserDefaultUILanguage()（0x0804 中文系 → ZhCn，否则 En）。
#include "core/i18n.h"

#include <windows.h>
#include <unordered_map>
#include <vector>

namespace i18n {
namespace {

struct Entry {
    const char* id;
    const wchar_t* zh;
    const wchar_t* en;
};

// 完整文案表（新增界面文案必须同时给 zh/en 两列）
const Entry kTable[] = {
    // ---- 应用 ----
    {"app.title",            L"全文查找",                       L"TextSearch"},
    {"msg.create_fail",      L"创建主窗口失败",                 L"Failed to create main window"},
    {"err",                  L"错误",                           L"Error"},
    {"note",                 L"提示",                           L"Note"},

    // ---- 顶部输入条 ----
    {"label.path",           L"路径：",                         L"Path:"},
    {"btn.browse",           L"浏览…",                          L"Browse…"},
    {"cue.path",             L"输入或浏览搜索目录…",            L"Type or browse a search directory…"},
    {"label.keyword",        L"关键词：",                       L"Keyword:"},
    {"cue.keyword",          L"输入关键词或正则表达式…",        L"Type a keyword or regex…"},
    {"btn.search",           L"搜索",                           L"Search"},
    {"btn.cancel",           L"取消",                           L"Cancel"},
    {"btn.collect",          L"收集扩展名",                     L"Collect Exts"},
    {"btn.export",           L"导出结果",                       L"Export"},
    {"btn.index",            L"建立索引",                       L"Build Index"},

    // ---- 选项条 ----
    {"opt.open",             L"▾ 选项",                         L"▾ Options"},
    {"opt.closed",           L"▸ 选项",                         L"▸ Options"},
    {"chk.recursive",        L"递归",                           L"Recursive"},
    {"chk.regex",            L"正则",                           L"Regex"},
    {"chk.case",             L"区分大小写",                     L"Match Case"},
    {"chk.wholeword",        L"全词",                           L"Whole Word"},
    {"chk.realtime",         L"强制即时扫描",                   L"Realtime"},
    {"lbl.threshold",        L"阈值",                           L"Threshold"},
    {"lbl.wildcard",         L"通配",                           L"Wildcard"},
    {"lbl.blacklist",        L"黑名单",                         L"Blacklist"},
    {"lbl.lang",             L"语言",                           L"Language"},
    {"lang.system",          L"跟随系统",                       L"System default"},
    {"lang.zh",              L"简体中文",                       L"简体中文"},
    {"lang.en",              L"English",                        L"English"},
    {"msg.lang_restart",     L"语言设置将在重启程序后生效。",   L"Language change takes effect after restart."},
    {"msg.lang_restart_t",   L"语言",                           L"Language"},

    // ---- 三栏 / 标题 ----
    {"label.ext",            L"扩展名（收集后勾选）",           L"Extensions"},
    {"label.result",         L"结果",                           L"Results"},
    {"label.result_capped",  L"结果（已达上限，结果被截断）",   L"Results (cap reached)"},
    {"label.preview",        L"预览（双击结果查看命中行）",     L"Preview (double-click = open)"},
    {"chk.wrap",             L"自动换行",                       L"Word Wrap"},
    {"col.ext",              L"扩展名",                         L"Extension"},
    {"col.path",             L"文件路径",                       L"File Path"},
    {"col.line",             L"行号",                           L"Line"},
    {"col.hit",              L"命中行",                         L"Matched Line"},
    {"preview.title",        L"预览：{1}  [{2}]",               L"Preview: {1}  [{2}]"},
    {"preview.hits",         L"  ·  {1} 处命中",                L"  ·  {1} hit(s)"},
    {"preview.hl",           L"（标黄 {1} 处）",                L"({1} highlighted)"},
    {"preview.fail",         L"预览失败：{1}",                  L"Preview failed: {1}"},

    // ---- 状态栏 ----
    {"sb.count_files",       L"{1} / {2} 文件",                 L"{1} / {2} files"},
    {"sb.count_index",       L"索引查询",                       L"Index query"},
    {"sb.index_ok",          L"索引：已建立",                   L"Index: built"},
    {"sb.index_no",          L"索引：未建立",                   L"Index: none"},
    {"sb.index_nopath",      L"索引：未选择路径",               L"Index: no path"},
    {"sb.index_badpath",     L"索引：路径无效",                 L"Index: invalid path"},
    {"status.scanning",      L"正在扫描…",                      L"Scanning…"},
    {"status.indexing",      L"正在建立索引…",                  L"Building index…"},
    {"status.mode_index",    L"（索引模式）",                   L"(index mode)"},
    {"status.mode_rt",       L"（实时扫描）",                   L"(realtime scan)"},
    {"status.done",          L"已完成",                         L"Done"},
    {"status.cancelled",     L"已取消（结果为部分）",           L"Cancelled (partial)"},
    {"status.hits",          L"（命中 {1} 条",                  L"(hit {1}"},
    {"status.skipped",       L"，跳过 {1} 个文件",              L", {1} files skipped"},
    {"status.scan_hits",     L"扫描中…  命中 {1} 条",           L"Scanning…  {1} hit(s)"},
    {"status.indexing_files",L"建索引 {1} / {2} 文件",          L"Indexing {1} / {2} files"},

    // ---- 校验 / 提示 / 错误 ----
    {"err.no_path",          L"错误：请填写搜索路径",           L"Error: please set a search path"},
    {"err.bad_path",         L"错误：路径不存在或不是目录",     L"Error: path missing or not a directory"},
    {"err.no_keyword",       L"错误：请输入搜索关键词",         L"Error: please enter a keyword"},
    {"hint.long_kw",         L"提示：关键词超过 1000 字符，已截断", L"Hint: keyword over 1000 chars, truncated"},
    {"warn.long_regex",      L"警告：正则表达式较长，可能影响性能", L"Warning: long regex may be slow"},
    {"err.regex",            L"正则语法错误：{1}",              L"Regex syntax error: {1}"},
    {"hint.no_ext",          L"提示：未勾选扩展名，使用常用扩展名搜索", L"Hint: no extensions checked, using common list"},
    {"err.no_ext",           L"错误：未指定搜索的扩展名，请先收集扩展名或配置常用扩展名", L"Error: no extensions — collect or configure the common list",},
    {"hint.threshold",       L"提示：大文件阈值无效（需为整数且 ≥1），已按 {1} MB 处理", L"Hint: invalid big-file threshold (integer ≥1), using {1} MB"},
    {"warn.capped",          L"结果已达上限 ({1})，已停止收集，请缩小搜索范围", L"Cap reached ({1}) — stopped; narrow the search",},
    {"warn.ext_missing",     L"警告: 勾选的 {1} 种类型中有 {2} 种不在索引内（{3}），这些文件搜不到；如需包含请重建索引", L"Warning: {2} of {1} types not in index ({3}) — those files are skipped; rebuild to include",},
    {"status.synced",        L"索引已同步 {1} 个变更文件",      L"Index synced {1} changed file(s)"},
    {"err.index_corrupt",    L"索引已损坏，已回退即时扫描，建议重建", L"Index corrupted, realtime fallback, rebuild advised"},
    {"err.no_files",         L"路径下未找到匹配文件: {1}",      L"No matching files under: {1}"},

    // ---- 建索引 / 取消 ----
    {"status.cancel_build",  L"正在取消…",                      L"Cancelling…"},
    {"status.cancel_build2", L"正在取消…（若建库已开始则无法中断，将照常完成）", L"Cancelling… (a started build cannot be interrupted and will finish)"},
    {"status.cancelled_idx", L"已取消建立索引",                 L"Index build cancelled"},
    {"status.index_done",    L"索引已建立（耗时 {1}），后续搜索本路径将会自动启用索引模式。注意：本次建立的索引，只针对建索引时纳入的扩展名（{2} 种）生效。", L"Index built (took {1}); searches here now use index mode. Covers only the extensions built ({2})."},
    {"err.index_fail",       L"建立索引失败：磁盘不可写/空间不足/权限不足（详见日志）", L"Index build failed: disk unwritable/full/permission denied (see log)"},
    {"err.index_exc",        L"建立索引异常: {1}",              L"Index build exception: {1}"},
    {"err.index_unk",        L"建立索引未知异常",               L"Unknown index build exception"},
    {"status.collecting",    L"正在收集扩展名…",                L"Collecting extensions…"},
    {"status.collected",     L"已收集并勾选扩展名，可开始搜索", L"Extensions collected and checked, ready to search"},
    {"status.collect_cancel",L"已取消收集扩展名",               L"Extension collection cancelled"},
    {"fmt.dur_sec",          L"{1}秒",                          L"{1}s"},
    {"fmt.dur_min",          L"{1}分{2}秒",                     L"{1}m{2}s"},
    {"status.indexed_files", L"已索引 {1} 文件",                L"{1} files indexed"},

    // ---- 复制 / 打开 / 导出 ----
    {"status.copied_line",   L"已复制选中行",                   L"Row copied"},
    {"err.clipboard",        L"复制失败：剪贴板被其他程序占用", L"Copy failed: clipboard busy"},
    {"status.copied_path",   L"已复制完整路径",                 L"Full path copied"},
    {"status.copied_line_no",L"已复制行号",                     L"Line number copied"},
    {"err.clipboard2",       L"复制失败：剪贴板被其他程序占用（{1}）", L"Copy failed: clipboard busy ({1})"},
    {"err.gone",             L"无法打开：文件不存在（可能已被删除或移动）", L"Cannot open: file missing (deleted or moved)"},
    {"err.shell",            L"打开所在文件夹失败（ShellExecute 错误 {1}）", L"Open folder failed (ShellExecute error {1})"},
    {"ctx.preview",          L"查看(&V)",                       L"&View"},
    {"ctx.open_folder",      L"打开所在文件夹(&O)",             L"Open Containing &Folder"},
    {"ctx.open_edit",        L"打开编辑(&E)",                   L"Open in &Editor"},
    {"ctx.copy_path",        L"复制完整路径(&P)",               L"Copy &Full Path"},
    {"ctx.copy_line_no",     L"复制行号(&N)",                   L"Copy Line &Number"},
    {"ctx.copy_all",         L"复制「路径:行号 + 内容」(&C)",   L"Copy Path:Line + Text (&C)"},
    {"mb.no_default_editor", L"没有找到「{1}」的默认打开工具，已改用记事本打开。", L"No default tool for \"{1}\" — opened with Notepad instead."},
    {"mb.open_fail",         L"无法打开文件（ShellExecute 错误 {1}）", L"Cannot open file (ShellExecute error {1})"},
    {"mb.no_export",         L"没有可导出的结果",               L"Nothing to export"},
    {"mb.export_title",      L"导出结果",                       L"Export Results"},
    {"mb.export_fail",       L"无法创建文件，请检查路径与权限", L"Cannot create file, check path and permissions"},
    {"mb.export_fail_title", L"导出失败",                       L"Export Failed"},
    {"status.exported",      L"已导出 {1} 条结果到 {2}",        L"Exported {1} result(s) to {2}"},

    // ---- 对话框（建索引确认 / 看门狗 / 关窗） ----
    {"mb.no_path",           L"请先填写有效的搜索路径",         L"Please set a valid search path first"},
    {"mb.no_ext",            L"未指定扩展名，请先收集扩展名或配置常用扩展名", L"No extensions specified, collect or configure the common list first"},
    {"mb.index_title",       L"建立索引",                       L"Build Index"},
    {"mb.index_summary",     L"将建立索引：\n目录：{1}\n扩展名（{2} 种）：{3}", L"Build index:\nDirectory: {1}\nExtensions ({2}): {3}"},
    {"mb.index_fallback",    L"\n\n警告：左栏未勾选任何扩展名，将按常用扩展名（{1} 种）全量建立索引，索引体积与建库时间可能较大。\n如只需索引少量类型，请先在左栏勾选。", L"\n\nWarning: no extensions checked — the full common list ({1} types) will be indexed; size and time may be large.\nTo index only a few types, check them in the left pane first."},
    {"mb.cancelled_ok",      L"已取消建立索引",                 L"Index build cancelled"},
    {"watchdog.title",       L"看门狗",                         L"Watchdog"},
    {"watchdog.msg",         L"搜索可能已停滞（60 秒无进度反馈），是否取消？", L"Search may be stalled (60s without progress). Cancel?"},
    {"close_build.title",    L"正在建立索引",                   L"Building Index"},
    {"close_build.msg",      L"正在建立索引（开始后无法中断）。\n关闭窗口将等待建库完成后自动退出，大目录可能需要较长时间。\n仍要关闭吗？", L"Index build in progress (cannot be interrupted).\nClosing will wait for the build to finish; large directories take a while.\nClose anyway?"},

    // ---- 搜索历史 ----
    {"hist.empty",           L"（本次会话还没有搜索历史）",     L"(no history this session)"},
    {"hist.note",            L"仅本次会话有效，退出后清空（关键词不保存）", L"Session only, cleared on exit"},

    // ---- 查看器 ----
    {"view.missing",         L"（无法打开：文件不存在。可能已被删除、移动或重命名，索引尚未更新——重建索引可清除这条陈旧结果。）", L"(Cannot open: file missing. It may have been deleted, moved or renamed and the index is not updated yet — rebuilding the index clears this stale result.)"},
    {"view.too_big",         L"（文件过大，未在预览中载入。请用外部编辑器打开本文件查看命中行。）", L"(File too large for preview. Open it in an external editor to see the hit.)"},
    {"view.empty",           L"（文件为空，没有可显示的内容）", L"(Empty file, nothing to show)"},
    {"view.no_access",       L"（无法打开：没有读取权限，或文件正被其他程序独占占用。）", L"(Cannot open: no read permission, or the file is exclusively locked.)"},
    {"view.window_note",     L"（大文件，仅显示第 {1} ~ {2} 行，范围内标黄）", L"(Large file, lines {1}~{2}, highlighted)"},
    {"view.window_note_no",  L"（大文件，仅显示第 {1} ~ {2} 行，已定位到命中行）", L"(Large file, lines {1}~{2}, at the hit)"},
    {"view.enc_unknown",     L"未知",                           L"Unknown"},

    // ---- 收集扩展名对话框 ----
    {"dext.title",           L"收集扩展名",                     L"Collect Extensions"},
    {"dext.hint",            L"已收集以下扩展名，勾选要纳入搜索的项：", L"Extensions found — check the ones to search:"},
    {"dext.manual",          L"手动添加：",                     L"Manual add:"},
    {"btn.add",              L"添加",                           L"Add"},
    {"btn.edit_wl",          L"编辑常用扩展名…",                L"Edit Common Extensions…"},
    {"btn.selectall",        L"全选",                           L"Select All"},
    {"btn.selectnone",       L"全不选",                         L"Select None"},
    {"btn.ok",               L"确定",                           L"OK"},
    {"btn.cancel",           L"取消",                           L"Cancel"},

    // ---- 编辑常用扩展名对话框 ----
    {"dwl.title",            L"编辑常用扩展名（常见文本文件类型）", L"Edit Common Extensions (text file types)"},
    {"dwl.hint",             L"每行一个扩展名（也可用逗号/空格分隔）。这些类型在「收集扩展名」时会默认勾选并排在最前面。保存后写入配置文件，也可直接用记事本编辑。", L"One extension per line (comma/space works). Pre-checked and sorted first in \"Collect Exts\". Saved to the config."},
    {"btn.reset",            L"重置为默认",                     L"Reset to Defaults"},
    {"dwl.err_empty",        L"常用扩展名不能为空，请至少保留一个扩展名。", L"The common list cannot be empty — keep at least one extension."},
    {"dwl.err_invalid",      L"以下条目格式无效已忽略：",       L"Invalid entries ignored:"},
    {"dwl.err_many",         L"…等共 {1} 项",                   L"… {1} in total"},
    {"dwl.err_save",         L"保存失败，请检查配置文件是否可写。", L"Save failed — check that the config file is writable."},
};

const int kTableN = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));

Lang g_lang = Lang::ZhCn;
std::unordered_map<std::string, int> g_index;   // id → 表内下标（启动时构建）

void build_index() {
    g_index.reserve(kTableN * 2);
    for (int i = 0; i < kTableN; ++i) g_index[kTable[i].id] = i;
}

}  // namespace

void set_language_from(const std::wstring& cfg) {
    // System → 按 UI 语言解析（0x0804 中文系 → ZhCn，其余 En）
    std::wstring v = cfg;
    if (v.empty() || v == L"system") {
        LANGID ui = GetUserDefaultUILanguage();
        g_lang = (PRIMARYLANGID(ui) == LANG_CHINESE) ? Lang::ZhCn : Lang::EnUs;
    } else if (v == L"en") {
        g_lang = Lang::EnUs;
    } else {
        g_lang = Lang::ZhCn;
    }
    build_index();
}

Lang language() { return g_lang; }

const wchar_t* tr(const char* id) {
    if (g_index.empty()) build_index();
    auto it = g_index.find(id);
    if (it == g_index.end()) {
        // 无条目 → id 本身（ASCII，开发期可见缺口）；缓存供立即使用
        static std::wstring fallback;
        fallback.assign(id, id + strlen(id));
        return fallback.c_str();
    }
    const Entry& e = kTable[it->second];
    return (g_lang == Lang::EnUs) ? e.en : e.zh;
}

std::wstring trf(const char* id, const std::wstring& a,
                 const std::wstring& b, const std::wstring& c) {
    std::wstring out = tr(id);
    const std::wstring reps[3] = {a, b, c};
    for (int n = 3; n >= 1; --n) {   // {10} 不存在但先替换高位防 {1} 吞 {10}
        wchar_t tok[8];
        swprintf(tok, 8, L"{%d}", n);
        const std::wstring& val = reps[n - 1];
        if (val.empty()) continue;
        size_t p = 0;
        while ((p = out.find(tok, p)) != std::wstring::npos) {
            out.replace(p, wcslen(tok), val);
            p += val.size();
        }
    }
    return out;
}

}  // namespace i18n
