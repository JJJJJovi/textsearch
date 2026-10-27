// gui/dlg_whitelist.cpp — 「常用扩展名」编辑对话框实现
//
// 让用户自行维护常见文本文件扩展名列表（原为程序内置，不客观）。
// UI 上统一称「常用扩展名」（"白名单"易被误解为屏蔽名单/安全白名单）；
// 代码内部标识符沿用 whitelist（英文无歧义），二者对应关系见本文件与各头文件注释。
//
// 数据存于 %LOCALAPPDATA%/TextSearch/textsearch.ini 的 [Extensions] common，
// 也可直接用记事本编辑该 ini 文件。
// 作用：收集扩展名时命中此列表的项会默认勾选，并排在列表最前面。
//
// 界面：说明标签 + 多行编辑框（每行一个扩展名）+ 重置为默认 / 确定 / 取消
// 字体与 DPI 处理同 dlg_extensions（微软雅黑 UI + 递归设字体 + DPI 缩放）。
#include "gui/dlg_whitelist.h"

#include "core/extension_manager.h"
#include "core/i18n.h"
#include "core/util.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace dlg_whitelist {

namespace {

enum {
    IDC_WL_EDIT = 3001,
    IDC_WL_RESET_BTN,
    IDC_WL_OK_BTN,
    IDC_WL_CANCEL_BTN,
    IDC_WL_HINT,
};

struct DialogState {
    std::set<std::wstring> result;
    bool confirmed = false;
    HINSTANCE hInst = nullptr;
    HWND hEdit = nullptr;
    HFONT hFont = nullptr;
    int dpi = 96;
};

static int dscale(const DialogState* s, int v) {
    return MulDiv(v, s->dpi, 96);
}

static BOOL CALLBACK SetFontProc(HWND hw, LPARAM lp) {
    SendMessageW(hw, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
    return TRUE;
}

static HFONT create_ui_font(int dpi) {
    int font_h = -MulDiv(12, dpi, 96);
    HFONT f = CreateFontW(font_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                          L"Microsoft YaHei UI");
    return f ? f : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// 规范化：小写 + 前导点
static std::wstring normalize_ext(const std::wstring& ext) {
    std::wstring r = ext;
    if (!r.empty() && r[0] != L'.') r = L"." + r;
    return to_lower_wstring(r);
}

static std::wstring trim_ws(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto isws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (b < e && isws(s[b])) ++b;
    while (e > b && isws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 解析编辑框内容：支持换行 / 逗号 / 空格 分隔；返回规范化后的集合与非法项
static std::set<std::wstring> parse_input(const std::wstring& text,
                                          std::vector<std::wstring>& bad) {
    std::set<std::wstring> out;
    // 统一分隔符为换行
    std::wstring t = text;
    for (auto& c : t) {
        if (c == L',' || c == L';' || c == L'\t') c = L'\n';
    }
    std::wstring cur;
    auto flush = [&]() {
        std::wstring item = trim_ws(cur);
        cur.clear();
        if (item.empty()) return;
        std::wstring e = normalize_ext(item);
        // 校验：点后仅允许 字母数字 _ + - #
        bool ok = e.size() >= 2 && e.size() <= 32;
        for (size_t i = 1; ok && i < e.size(); ++i) {
            wchar_t c = e[i];
            bool alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
            if (!alnum && c != L'_' && c != L'+' && c != L'-' && c != L'#') ok = false;
        }
        if (ok) out.insert(e);
        else bad.push_back(item);
    };
    for (wchar_t c : t) {
        if (c == L'\n') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

// 集合 → 编辑框文本（每行一个）
static std::wstring format_list(const std::set<std::wstring>& s) {
    std::wstring out;
    for (const auto& e : s) {
        if (!out.empty()) out += L"\r\n";
        out += e;
    }
    return out;
}

// 取编辑框文本
static std::wstring get_edit_text(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return L"";
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hEdit, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    return buf;
}

LRESULT CALLBACK DlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogState* state = reinterpret_cast<DialogState*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            state = reinterpret_cast<DialogState*>(
                reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            RECT crc{};
            GetClientRect(hWnd, &crc);
            const int CW = crc.right - crc.left;
            const int CH = crc.bottom - crc.top;
            const int M = dscale(state, 12);
            const int GAP = dscale(state, 8);
            const int CTL_H = dscale(state, 30);
            const int BTN_W = dscale(state, 96);
            const int content_w = CW - M * 2;

            const int hint_h = dscale(state, 44);
            const int btn_y = CH - M - CTL_H;
            const int edit_y = M + hint_h + GAP;
            const int edit_h = btn_y - edit_y - dscale(state, 12);

            CreateWindowExW(0, L"STATIC",
                i18n::tr("dwl.hint"),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                M, M, content_w, hint_h, hWnd,
                reinterpret_cast<HMENU>(IDC_WL_HINT), state->hInst, nullptr);

            state->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_WANTRETURN | WS_VSCROLL,
                M, edit_y, content_w, edit_h, hWnd,
                reinterpret_cast<HMENU>(IDC_WL_EDIT), state->hInst, nullptr);

            // 底部：重置为默认（左），确定 / 取消（右）
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.reset"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                M, btn_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_WL_RESET_BTN), state->hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.cancel"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                CW - M - BTN_W, btn_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_WL_CANCEL_BTN), state->hInst, nullptr);
            CreateWindowExW(0, L"BUTTON", i18n::tr("btn.ok"),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                CW - M - BTN_W * 2 - GAP, btn_y, BTN_W, CTL_H, hWnd,
                reinterpret_cast<HMENU>(IDC_WL_OK_BTN), state->hInst, nullptr);

            // 字体：递归设置给所有子控件
            state->hFont = create_ui_font(state->dpi);
            SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->hFont), TRUE);
            EnumChildWindows(hWnd, &SetFontProc, reinterpret_cast<LPARAM>(state->hFont));

            SetFocus(state->hEdit);
            return 0;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            if (id == IDC_WL_RESET_BTN) {
                // 重置为内置默认列表（仅改编辑框内容，点确定后才写盘）
                SetWindowTextW(state->hEdit, format_list(default_whitelist()).c_str());
                SetFocus(state->hEdit);
                return 0;
            }
            if (id == IDC_WL_OK_BTN) {
                std::vector<std::wstring> bad;
                std::set<std::wstring> parsed = parse_input(get_edit_text(state->hEdit), bad);
                if (parsed.empty()) {
                    MessageBoxW(hWnd, i18n::tr("dwl.err_empty"),
                                i18n::tr("dwl.title"), MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (!bad.empty()) {
                    std::wstring msg = i18n::tr("dwl.err_invalid") +
                                       std::wstring(L"\n");
                    for (size_t i = 0; i < bad.size() && i < 10; ++i) {
                        msg += bad[i] + L"\n";
                    }
                    if (bad.size() > 10)
                        msg += i18n::trf("dwl.err_many",
                                         std::to_wstring(bad.size()));
                    MessageBoxW(hWnd, msg.c_str(), i18n::tr("dwl.title"),
                                MB_OK | MB_ICONINFORMATION);
                }
                // 写回配置文件
                ExtensionManager mgr;
                if (!mgr.save_whitelist(parsed)) {
                    MessageBoxW(hWnd, i18n::tr("dwl.err_save"),
                                i18n::tr("dwl.title"), MB_OK | MB_ICONERROR);
                    return 0;
                }
                state->result = parsed;
                state->confirmed = true;
                DestroyWindow(hWnd);
                return 0;
            }
            if (id == IDC_WL_CANCEL_BTN) {
                state->confirmed = false;
                DestroyWindow(hWnd);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            state->confirmed = false;
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY: {
            if (state && state->hFont) {
                HFONT stock = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
                if (state->hFont != stock) DeleteObject(state->hFont);
                state->hFont = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

} // namespace

bool show(HWND parent, HINSTANCE hInst,
          const std::set<std::wstring>& current,
          std::set<std::wstring>& updated) {
    static const wchar_t* kClass = L"TextSearchWhitelistDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    DialogState state;
    state.result = current;
    state.hInst = hInst;
    // 优先取主窗口所在显示器的 DPI（多显示器下与主窗口一致），取不到退回屏幕 DC
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

    EnableWindow(parent, FALSE);
    HWND hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClass, i18n::tr("dwl.title"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(560, state.dpi, 96), MulDiv(460, state.dpi, 96),
        parent, nullptr, hInst, &state);

    if (!hWnd) {
        EnableWindow(parent, TRUE);
        return false;
    }

    // 初值：当前常用扩展名列表
    SetWindowTextW(state.hEdit, format_list(current).c_str());

    // 居中到父窗口
    RECT rcParent{}, rcDlg{};
    GetWindowRect(parent, &rcParent);
    GetWindowRect(hWnd, &rcDlg);
    int x = rcParent.left + ((rcParent.right - rcParent.left) -
                             (rcDlg.right - rcDlg.left)) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) -
                            (rcDlg.bottom - rcDlg.top)) / 2;
    SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(parent, TRUE);
    SetFocus(parent);

    if (state.confirmed) {
        updated = state.result;
        return true;
    }
    return false;
}

} // namespace dlg_whitelist
