// gui/dlg_extensions.cpp — 扩展名收集对话框实现（T5 5.4b）
#include "gui/dlg_extensions.h"
#include "gui/dlg_whitelist.h"
#include "core/extension_manager.h"
#include "core/i18n.h"
#include "core/util.h"
#include "core/logger.h"

#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace dlg_extensions {

namespace {

// 控件 ID
enum {
    IDC_EXT_LISTVIEW = 2001,
    IDC_SELECTALL_BTN,
    IDC_SELECTNONE_BTN,
    IDC_OK_BTN,
    IDC_CANCEL_BTN,
    IDC_HINT_LABEL,
    IDC_MANUAL_EDIT,     // 手动添加：扩展名输入框
    IDC_ADD_BTN,         // 手动添加：「添加」按钮
    IDC_EDIT_WL_BTN,     // 编辑常用扩展名（打开「常用扩展名」编辑对话框）
};

// 对话框状态（透传给 WndProc）
struct DialogState {
    std::set<std::wstring> collected;   // 收集到的所有扩展名（有序）
    std::set<std::wstring> whitelist;   // 常用扩展名（预勾选用，且命中项排在列表最前）; 代码内沿用 whitelist
    std::set<std::wstring> result;      // 用户确认后的勾选集合
    bool confirmed = false;
    HINSTANCE hInst = nullptr;
    HWND hList = nullptr;
    HWND hLabel = nullptr;
    HWND hManualEdit = nullptr;         // 手动添加输入框
    HFONT hFont = nullptr;              // UI 字体（与主窗口一致：微软雅黑 UI + DPI 缩放）
    int dpi = 96;                       // 当前 DPI（用于控件尺寸缩放）
};

// DPI 缩放（与 MainWindow::scale_dpi 同口径）
static int dscale(const DialogState* s, int v) {
    return MulDiv(v, s->dpi, 96);
}

// 递归给所有子控件设置字体。
// ⚠️ 关键：WM_SETFONT 发给父窗口**不会**自动传给子控件（本对话框无资源模板，
// 不走 DS_SETFONT）。原实现只给对话框窗口本身设字体，导致所有子控件仍用系统
// 默认的 MS Sans Serif（位图字体、无 ClearType）→ 界面模糊老旧。这是"字体难看"的根因。
static BOOL CALLBACK SetFontProc(HWND hw, LPARAM lp) {
    SendMessageW(hw, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
    return TRUE;
}

// 创建 UI 字体（与主窗口同规范：微软雅黑 UI，12pt，DPI 缩放，ClearType 渲染）
static HFONT create_ui_font(int dpi) {
    int font_h = -MulDiv(12, dpi, 96);
    HFONT f = CreateFontW(font_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                          L"Microsoft YaHei UI");
    return f ? f : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// 将扩展名规范化为小写 + 前导点
std::wstring normalize_ext(const std::wstring& ext) {
    std::wstring result = ext;
    if (!result.empty() && result[0] != L'.') {
        result = L"." + result;
    }
    return to_lower_wstring(result);
}

// 去除首尾空白
std::wstring trim_ws(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(static_cast<wint_t>(s[b]))) ++b;
    while (e > b && iswspace(static_cast<wint_t>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 取文件扩展名（含前导点，小写）；无扩展名返回 L"."
std::wstring get_extension(const std::filesystem::path& p) {
    std::wstring ext = p.extension().wstring();
    if (ext.empty()) {
        // 无扩展名 → 用一个占位表示"无扩展名"
        return L"(无扩展名)";
    }
    return to_lower_wstring(ext);
}

// 列表项复选状态
bool is_item_checked(HWND hList, int idx) {
    return ListView_GetCheckState(hList, idx) != 0;
}

void set_item_checked(HWND hList, int idx, bool checked) {
    ListView_SetCheckState(hList, idx, checked ? TRUE : FALSE);
}

// 填充列表（checked_pre 为应勾选的集合：初值为常用扩展名∩收集，手动添加后为
// 「当前勾选 ∪ 新增项」，重填以保持用户已做的勾选/取消不被重置）
//
// 排序：**常用扩展名命中的排在最前面**（它们即默认勾选的项），组内按字母序。
// 这样用户无需在长列表里逐个查找哪些被默认勾上了。
void populate_list(HWND hList, const std::set<std::wstring>& exts,
                   const std::set<std::wstring>& checked_pre,
                   const std::set<std::wstring>& whitelist) {
    std::vector<std::wstring> ordered(exts.begin(), exts.end());
    std::stable_sort(ordered.begin(), ordered.end(),
        [&whitelist](const std::wstring& a, const std::wstring& b) {
            bool wa = whitelist.count(a) != 0;
            bool wb = whitelist.count(b) != 0;
            if (wa != wb) return wa;   // 常用扩展名命中优先
            return a < b;              // 其次字母序（"(无扩展名)" 以 '(' 开头，自然排在后面）
        });

    ListView_DeleteAllItems(hList);
    int idx = 0;
    for (const auto& e : ordered) {
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = idx;
        lvi.iSubItem = 0;
        std::wstring text = e;
        lvi.pszText = const_cast<LPWSTR>(text.c_str());
        ListView_InsertItem(hList, &lvi);
        bool pre = (checked_pre.find(e) != checked_pre.end());
        set_item_checked(hList, idx, pre);
        ++idx;
    }
}

// 收集结果（勾选项）：直接按列表项文本 + 复选状态读取，
// 不依赖「collected 集合顺序 == 列表顺序」的隐含约定，插入新项后依然正确。
std::set<std::wstring> gather_checked(HWND hList) {
    std::set<std::wstring> result;
    int n = ListView_GetItemCount(hList);
    for (int i = 0; i < n; ++i) {
        if (!is_item_checked(hList, i)) continue;
        wchar_t buf[64];
        ListView_GetItemText(hList, i, 0, buf, 64);
        std::wstring e = buf;
        // 排除占位"无扩展名"
        if (e == L"(无扩展名)") {
            result.insert(L"");  // 空扩展名
        } else if (!e.empty()) {
            result.insert(e);
        }
    }
    return result;
}

// 手动添加扩展名：校验输入（非空、以点开头、仅常见扩展名字符），
// 返回 false 时 reason 给出提示文本。扩展名规范化为小写 + 前导点。
bool validate_manual_ext(const std::wstring& raw, std::wstring& out,
                         std::wstring& reason) {
    std::wstring t = trim_ws(raw);
    if (t.empty()) {
        reason = L"请输入扩展名（如 .xyz 或 xyz）";
        return false;
    }
    std::wstring ext = normalize_ext(t);
    if (ext.size() < 2 || ext.size() > 32) {
        reason = L"扩展名格式无效（应为 .xxx 形式，1~31 个字符）";
        return false;
    }
    for (size_t i = 1; i < ext.size(); ++i) {
        wchar_t c = ext[i];
        bool ok = iswalnum(static_cast<wint_t>(c)) ||
                  c == L'_' || c == L'+' || c == L'-' || c == L'#';
        if (!ok) {
            reason = L"扩展名含有非法字符：" + std::wstring(1, c);
            return false;
        }
    }
    out = ext;
    return true;
}

// 手动添加输入框子类化：Enter 触发「添加」而非对话框默认按钮（确定），
// 避免刚输入的扩展名还没加入就被整框提交丢失。
LRESULT CALLBACK ManualEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR /*uIdSubclass*/, DWORD_PTR /*refData*/) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        PostMessageW(GetParent(hWnd), WM_COMMAND,
                     MAKEWPARAM(IDC_ADD_BTN, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

LRESULT CALLBACK DlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            state = reinterpret_cast<DialogState*>(
                reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(state));

            // ---- 布局：按客户区精确计算，统一 DPI 缩放 ----
            RECT crc{};
            GetClientRect(hWnd, &crc);
            const int CW = crc.right - crc.left;    // 客户区宽
            const int CH = crc.bottom - crc.top;    // 客户区高
            const int M  = dscale(state, 12);       // 外边距
            const int GAP = dscale(state, 8);       // 组内间距
            const int GGAP = dscale(state, 10);     // 区之间间距
            const int LBL_H = dscale(state, 20);    // 标签高
            const int CTL_H = dscale(state, 30);    // 输入/按钮高
            const int BTN_W = dscale(state, 88);    // 按钮宽
            const int ADD_W = dscale(state, 64);    // 「添加」按钮宽

            const int content_w = CW - M * 2;

            // 提示标签
            CreateWindowExW(0, L"STATIC", i18n::tr("dext.hint"),
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            M, M, content_w, LBL_H, hWnd,
                            reinterpret_cast<HMENU>(IDC_HINT_LABEL),
                            state->hInst, nullptr);

            // 手动添加行（先算高度，列表高度 = 客户区剩余空间）
            const int manual_y = CH - M - CTL_H;
            const int bottom_y = manual_y - GGAP - CTL_H;

            // 扩展名列表（带复选框）：占据中间全部剩余空间，随窗口高度自适应
            const int list_y = M + LBL_H + GAP;
            const int list_h = bottom_y - list_y - GGAP;
            state->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT |
                    LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_VSCROLL | WS_HSCROLL,
                M, list_y, content_w, list_h, hWnd,
                reinterpret_cast<HMENU>(IDC_EXT_LISTVIEW),
                state->hInst, nullptr);
            ListView_SetExtendedListViewStyle(state->hList,
                LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            // 列宽自适应列表客户区（留出复选框与垂直滚动条空间）
            {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = content_w - dscale(state, 28);
                std::wstring coltext = i18n::tr("col.ext");
                col.pszText = const_cast<LPWSTR>(coltext.c_str());
                ListView_InsertColumn(state->hList, 0, &col);
            }

            // 初始勾选 = 常用扩展名 ∩ 收集结果
            std::set<std::wstring> pre;
            for (const auto& e : state->collected) {
                if (state->whitelist.count(e)) pre.insert(e);
            }
            populate_list(state->hList, state->collected, pre, state->whitelist);

            // 手动添加行：目录中不存在的扩展名（如 .xyz）也可手打加入
            const int lbl_w = dscale(state, 76);
            CreateWindowExW(0, L"STATIC", i18n::tr("dext.manual"),
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            M, manual_y + dscale(state, 5), lbl_w, LBL_H, hWnd,
                            nullptr, state->hInst, nullptr);
            const int edit_x = M + lbl_w;
            const int edit_w = content_w - lbl_w - ADD_W - GAP;
            state->hManualEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                edit_x, manual_y, edit_w, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_MANUAL_EDIT),
                state->hInst, nullptr);
            SetWindowSubclass(state->hManualEdit, &ManualEditProc, 0, 0);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.add"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                edit_x + edit_w + GAP, manual_y, ADD_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_ADD_BTN),
                state->hInst, nullptr);

            // 底部按钮：辅助操作靠左（编辑常用扩展名 / 全选 / 全不选），主操作靠右（确定/取消）
            const int ew_w = dscale(state, 104);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.edit_wl"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                M, bottom_y, ew_w, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_EDIT_WL_BTN),
                state->hInst, nullptr);
            const int sel_x = M + ew_w + GAP;
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.selectall"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                sel_x, bottom_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_SELECTALL_BTN),
                state->hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.selectnone"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                sel_x + BTN_W + GAP, bottom_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_SELECTNONE_BTN),
                state->hInst, nullptr);

            // 确定 / 取消：右对齐（取消在右、确定在其左，符合 Windows 惯例）
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.cancel"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                CW - M - BTN_W, bottom_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_CANCEL_BTN),
                state->hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.ok"),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                CW - M - BTN_W * 2 - GAP, bottom_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_OK_BTN),
                state->hInst, nullptr);

            // ---- 字体：递归设置给所有子控件（含列表、编辑框、按钮、标签）----
            // 必须在所有子控件创建完成后调用，否则后创建的控件拿不到字体。
            state->hFont = create_ui_font(state->dpi);
            SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->hFont), TRUE);
            EnumChildWindows(hWnd, &SetFontProc,
                             reinterpret_cast<LPARAM>(state->hFont));

            // 焦点给手动添加输入框，方便直接键入扩展名
            SetFocus(state->hManualEdit);
            return 0;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            switch (id) {
                case IDC_SELECTALL_BTN: {
                    int n = ListView_GetItemCount(state->hList);
                    for (int i = 0; i < n; ++i) set_item_checked(state->hList, i, true);
                    return 0;
                }
                case IDC_SELECTNONE_BTN: {
                    int n = ListView_GetItemCount(state->hList);
                    for (int i = 0; i < n; ++i) set_item_checked(state->hList, i, false);
                    return 0;
                }
                case IDC_ADD_BTN: {
                    // 手动添加：读取输入框 → 校验/规范化 → 加入集合并勾选
                    int len = GetWindowTextLengthW(state->hManualEdit);
                    if (len <= 0) return 0;
                    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
                    GetWindowTextW(state->hManualEdit, buf.data(), len + 1);
                    buf.resize(static_cast<size_t>(len));

                    std::wstring ext, reason;
                    if (!validate_manual_ext(buf, ext, reason)) {
                        MessageBoxW(hWnd, reason.c_str(), L"添加扩展名",
                                    MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    // 保存当前勾选状态，插入新项后整表重填（保持既有勾选不被重置）
                    std::set<std::wstring> cur_checked = gather_checked(state->hList);
                    state->collected.insert(ext);
                    cur_checked.insert(ext);   // 新增项默认勾选
                    populate_list(state->hList, state->collected, cur_checked,
                                  state->whitelist);
                    SetWindowTextW(state->hManualEdit, L"");
                    SetFocus(state->hManualEdit);
                    return 0;
                }
                case IDC_EDIT_WL_BTN: {
                    // 编辑常用扩展名：打开独立对话框，保存后刷新列表
                    // （新列表中的项会重新排到最前并默认勾选）
                    std::set<std::wstring> updated = state->whitelist;
                    if (dlg_whitelist::show(hWnd, state->hInst,
                                            state->whitelist, updated)) {
                        state->whitelist = updated;
                        // 保持用户已做的勾选，仅重排 + 补勾新进入常用扩展名的项
                        std::set<std::wstring> cur_checked = gather_checked(state->hList);
                        for (const auto& e : state->collected) {
                            if (state->whitelist.count(e)) cur_checked.insert(e);
                        }
                        populate_list(state->hList, state->collected, cur_checked,
                                      state->whitelist);
                    }
                    SetFocus(state->hManualEdit);
                    return 0;
                }
                case IDC_OK_BTN: {
                    state->result = gather_checked(state->hList);
                    state->confirmed = true;
                    DestroyWindow(hWnd);
                    return 0;
                }
                case IDC_CANCEL_BTN: {
                    state->confirmed = false;
                    DestroyWindow(hWnd);
                    return 0;
                }
            }
            return 0;
        }

        case WM_CLOSE: {
            state->confirmed = false;
            DestroyWindow(hWnd);
            return 0;
        }

        case WM_DESTROY: {
            // 释放自创建的 UI 字体（GetStockObject 返回的不可删除，
            // 故仅当 CreateFontW 成功时删除；此处统一判断：非 stock 字体才删）
            if (state && state->hFont) {
                // DEFAULT_GUI_FONT 是 stock 对象，不能 DeleteObject。
                // create_ui_font 在 CreateFontW 失败时返回 stock 字体，
                // 这里用「是否等于 stock 对象」来区分，避免误删系统对象。
                HFONT stock = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                if (state->hFont != stock) {
                    DeleteObject(state->hFont);
                }
                state->hFont = nullptr;
            }
            PostQuitMessage(0);  // 退出模态消息循环
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

} // namespace

std::set<std::wstring> collect_all_extensions(const std::filesystem::path& root,
                                              bool recursive) {
    std::set<std::wstring> exts;
    namespace fs = std::filesystem;
    std::error_code ec;

    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) { ec.clear(); continue; }
                if (!entry.is_regular_file(ec)) continue;
                if (is_reparse_point(entry.path())) continue;
                std::wstring ext = get_extension(entry.path());
                exts.insert(ext);
            }
        } else {
            for (const auto& entry : fs::directory_iterator(
                     root, fs::directory_options::skip_permission_denied, ec)) {
                if (ec) { ec.clear(); continue; }
                if (!entry.is_regular_file(ec)) continue;
                std::wstring ext = get_extension(entry.path());
                exts.insert(ext);
            }
        }
    } catch (const std::exception& e) {
        logger::warn(L"收集扩展名异常: " + utf8_to_wstring(e.what()));
    } catch (...) {
        logger::warn(L"收集扩展名未知异常");
    }

    return exts;
}

bool show(HWND parent, HINSTANCE hInst,
          const std::filesystem::path& root,
          const std::set<std::wstring>& whitelist,
          std::set<std::wstring>& checked) {
    // 先收集扩展名
    std::set<std::wstring> collected = collect_all_extensions(root, true);

    // 注册对话框窗口类（仅一次）
    static const wchar_t* kDlgClass = L"TextSearchExtDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClass;
        RegisterClassW(&wc);
        registered = true;
    }

    DialogState state;
    state.collected = collected;
    state.whitelist = whitelist;
    state.hInst = hInst;

    // 优先取主窗口所在显示器的 DPI（多显示器 + 主窗口支持 WM_DPICHANGED 后，
    // 对话框应与主窗口所在屏一致）；取不到再退回屏幕 DC 的系统 DPI。
    {
        int d = 0;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using GetDpiProc = UINT(WINAPI*)(HWND);
            auto pGetDpi = reinterpret_cast<GetDpiProc>(
                reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
            if (pGetDpi && parent) d = static_cast<int>(pGetDpi(parent));
        }
        if (d <= 0) {
            HDC dc = GetDC(nullptr);
            if (dc) {
                d = GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(nullptr, dc);
            }
        }
        state.dpi = (d > 0) ? d : 96;
    }

    // 禁用父窗口（模态）
    EnableWindow(parent, FALSE);

    // 窗口尺寸随 DPI 缩放（基准 500×440 @96DPI）
    int win_w = MulDiv(500, state.dpi, 96);
    int win_h = MulDiv(440, state.dpi, 96);

    HWND hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kDlgClass, i18n::tr("dext.title"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
        parent, nullptr, hInst, &state);

    if (!hWnd) {
        EnableWindow(parent, TRUE);
        return false;
    }

    // 居中到父窗口
    RECT rcParent{}, rcDlg{};
    GetWindowRect(parent, &rcParent);
    GetWindowRect(hWnd, &rcDlg);
    int x = rcParent.left + ((rcParent.right - rcParent.left) -
                             (rcDlg.right - rcDlg.left)) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) -
                            (rcDlg.bottom - rcDlg.top)) / 2;
    SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // 注：字体已在 WM_CREATE 中创建并递归设置给所有子控件（见 DlgProc）

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // 模态消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // 对话框内的 Tab/Enter 不走 IsDialogMessage（无资源模板），手动处理
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // 窗口已销毁（PostQuitMessage 触发 GetMessage 返回 0）
    }

    // 恢复父窗口
    EnableWindow(parent, TRUE);
    SetFocus(parent);

    if (state.confirmed) {
        checked = state.result;
        return true;
    }
    return false;
}

} // namespace dlg_extensions
