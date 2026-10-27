// gui/main_window.cpp — 主窗口实现（T5 GUI + T6 整合）
#include "gui/main_window.h"
#include "gui/dlg_extensions.h"
#include "core/matcher.h"
#include "core/encoding.h"
#include "core/filesystem.h"
#include "core/util.h"
#include "core/logger.h"
#include "core/indexer.h"  // T4b 索引构建
#include "core/i18n.h"

#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <dwmapi.h>      // DwmSetWindowAttribute（沉浸式深色标题栏）
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER (WM_USER + 33)   // 编辑框水印提示（需 comctl32 v6）
#endif
#include <commdlg.h>
#include <ctime>
#include <fstream>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>   // ShellExecuteW（S7「打开所在文件夹」）
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <windowsx.h>   // Button_GetCheck / Button_SetCheck 等控件宏

// ---- 消息协议常量（T6 6.1）----
// WM_APP+1：进度（wParam=done, lParam=total）
// WM_APP+2：一批结果（lParam=vector<MatchResult>*，主线程消费后 delete）
// WM_APP+3：完成/错误（wParam=状态码 0=完成 1=错误，lParam=wstring* 或 0）
// WM_APP+4：跳过统计（wParam=skipped）
// WM_APP+5：索引状态异步检测结果（wParam=gen, lParam=1/0）
// WM_APP+6：搜索模式异步判定结果（lParam=1 索引 / 0 实时）— 在 worker 线程
//           预判后回传，避免在 UI 线程同步跑 Indexer::index_exists 卡界面
namespace {
constexpr UINT WM_PROGRESS     = WM_APP + 1;
constexpr UINT WM_RESULTS      = WM_APP + 2;
constexpr UINT WM_DONE         = WM_APP + 3;
constexpr UINT WM_SKIP         = WM_APP + 4;
constexpr UINT WM_INDEX_STATUS = WM_APP + 5;
constexpr UINT WM_SEARCH_MODE  = WM_APP + 6;
}  // namespace（仅承载 WM_* 私有消息常量）

// ListView 双缓冲样式（部分 MinGW 头未导出，显式兜底）
#ifndef LVS_EX_DOUBLEBUFFER
#define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

// 静态列表自绘：隔行浅灰底 + 浅蓝选中行（保持隔行层次感，避免整片深蓝）
// 另：S3 —— 当 results_partial_（取消后的部分结果）为真，整列文字置灰，
// 提醒用户「别据此下结论」。需访问成员 results_partial_，故改为成员函数。
DWORD MainWindow::list_view_custom_draw(LPARAM lParam) {
    auto* plvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
    switch (plvcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            int i = static_cast<int>(plvcd->nmcd.dwItemSpec);
            bool selected = (plvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
            // S3：仅结果列表（不是扩展名列表）在「取消后的部分结果」时整列置灰
            if (results_partial_ && plvcd->nmcd.hdr.hwndFrom == hResultList_) {
                plvcd->clrText = RGB(150, 150, 150);
                plvcd->clrTextBk = selected ? RGB(226, 228, 232)
                                            : (i % 2 == 1 ? RGB(244, 245, 248)
                                                         : RGB(255, 255, 255));
                return CDRF_NEWFONT;
            }
            if (selected) {
                // 浅蓝高亮（不遮盖隔行层次，多选时也不会糊成一片）
                plvcd->clrTextBk = RGB(210, 225, 250);
                plvcd->clrText   = RGB(20, 20, 25);
            } else if (i % 2 == 1) {
                plvcd->clrTextBk = RGB(244, 245, 248);  // 极浅灰（比之前更淡）
                plvcd->clrText   = RGB(32, 32, 40);
            }
            return CDRF_NEWFONT;
        }
    }
    return CDRF_DODEFAULT;
}

// 悬停滚轮转发：焦点在列表上时，光标若悬停在预览区，
// 滚轮转发给查看器；否则按原样滚动列表。返回值即转发/默认处理的结果。
LRESULT CALLBACK MainWindow::list_wheel_proc(HWND hWnd, UINT msg,
                                             WPARAM wParam, LPARAM lParam,
                                             UINT_PTR /*idSubclass*/,
                                             DWORD_PTR refData) {
    if (msg == WM_MOUSEWHEEL) {
        auto* self = reinterpret_cast<MainWindow*>(refData);
        if (self && self->viewer_.hwnd()) {
            DWORD pos = GetMessagePos();
            POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
            ScreenToClient(self->hwnd_, &pt);
            if (self->viewer_.contains_point(pt)) {
                return SendMessageW(self->viewer_.hwnd(), WM_MOUSEWHEEL,
                                    wParam, lParam);
            }
        }
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// 控件 ID
enum CtrlId {
    IDC_PATH_EDIT = 1001,
    IDC_BROWSE_BTN,
    IDC_KEYWORD_EDIT,
    IDC_SEARCH_BTN,
    IDC_CANCEL_BTN,
    IDC_COLLECT_BTN,
    IDC_EXPORT_BTN,
    IDC_INDEX_BTN,
    IDC_OPT_TOGGLE,
    IDC_CHK_RECURSIVE,
    IDC_CHK_REGEX,
    IDC_CHK_CASE,
    IDC_CHK_WHOLEWORD,
    IDC_CHK_REALTIME,       // 强制即时扫描（跳过索引，用于索引/实时结果比对）
    IDC_THRESHOLD_EDIT,
    IDC_WILDCARD_EDIT,
    IDC_BLACKLIST_EDIT,
    IDC_EXT_LIST,
    IDC_EXT_LABEL,
    IDC_RESULT_LIST,
    IDC_VIEWER_LABEL,
    IDC_STATUSBAR,
    IDC_PROGRESS,
    IDC_HISTORY_BTN,       // S5：关键词历史下拉按钮
    IDC_WRAP_CHK,          // 预览自动换行（显示折行，不改变真实行号）
    IDC_LANG_COMBO,        // 界面语言（重启生效）
};

// 加速键命令 ID
enum CmdId {
    CMD_FOCUS_PATH = 9001,
    CMD_FOCUS_KEYWORD,
    CMD_EXPORT,
    CMD_CANCEL,
    CMD_FOCUS_LEFT,
    CMD_FOCUS_MIDDLE,
    CMD_FOCUS_RIGHT,
    // S7：结果列表右键菜单
    CTX_PREVIEW,
    CTX_OPEN_FOLDER,
    CTX_OPEN_EDIT,
    CTX_COPY_PATH,
    CTX_COPY_LINENO,
    CTX_COPY_LINE,
};

constexpr UINT_PTR kWatchdogTimerId = 1;
constexpr ULONGLONG kWatchdogTimeoutMs = 60000;  // 60s 无进度（F.2.4）
constexpr UINT kWatchdogIntervalMs = 5000;       // 每 5s 检查一次
constexpr size_t kMaxPatternLen = 1000;          // F.1.4 关键词最大长度

// S5：关键词历史。仅存内存，绝不落盘（见 B2：配置文件永不保存关键词）。
constexpr size_t kMaxKeywordHistory = 10;
constexpr WORD  kHistoryCmdBase    = 9500;       // 历史菜单项命令 ID 起始

// 取控件文本
std::wstring get_window_text(HWND hWnd) {
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return std::wstring();
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    int got = GetWindowTextW(hWnd, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(got > 0 ? got : 0));
    return buf;
}

// 去除首尾空白
std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(static_cast<wint_t>(s[b]))) ++b;
    while (e > b && iswspace(static_cast<wint_t>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 关键词编辑框子类化：Enter 触发搜索（F.1.3）
LRESULT CALLBACK KeywordEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR /*uIdSubclass*/, DWORD_PTR refData) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        auto* win = reinterpret_cast<MainWindow*>(refData);
        if (win && win->hwnd()) {
            PostMessageW(win->hwnd(), WM_COMMAND,
                         MAKEWPARAM(IDC_SEARCH_BTN, BN_CLICKED), 0);
        }
        return 0;
    }
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// 编辑框垂直居中子类化：单行 Edit 文本从客户区顶部绘制，控件高度大于字体
// 行高时底部留白，视觉偏上。在 WM_NCCALCSIZE 中按实际字体度量裁掉顶部多余
// 客户区（extra/2），使文字真正居中。字体度量本身已随 DPI 缩放（12pt 按 dpi_
// 创建），此处无需再乘 scale_dpi。
LRESULT CALLBACK CenteredEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR /*uIdSubclass*/, DWORD_PTR /*refData*/) {
    if (msg == WM_NCCALCSIZE && wParam && lParam) {
        LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
        auto* rc = reinterpret_cast<RECT*>(lParam);
        HDC dc = GetDC(hWnd);
        if (dc) {
            HFONT f = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
            HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm)) {
                int extra = (rc->bottom - rc->top) - tm.tmHeight;
                if (extra > 0)
                    rc->top += extra / 2;   // 裁掉顶部一半留白 → 文字居中
            }
            if (old) SelectObject(dc, old);
            ReleaseDC(hWnd, dc);
        }
        return r;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// CSV 字段转义：含逗号/引号/换行则用双引号包裹，内部引号双写
std::wstring csv_escape(const std::wstring& field) {
    bool need_quote = field.find_first_of(L",\"\r\n") != std::wstring::npos;
    std::wstring out = field;
    if (need_quote) {
        std::wstring escaped;
        escaped.reserve(field.size() + 2);
        for (wchar_t c : field) {
            if (c == L'"') escaped += L"\"\"";
            else escaped.push_back(c);
        }
        out = L"\"" + escaped + L"\"";
    }
    return out;
}

// 毫秒 → 中文耗时串：<1秒 / Ns秒 / N分S秒
std::wstring format_duration_ms(long long ms) {
    if (ms < 1000) return L"<1秒";
    long long secs = ms / 1000;
    if (secs < 60) return std::to_wstring(secs) + L"秒";
    long long m = secs / 60;
    long long s = secs % 60;
    return std::to_wstring(m) + L"分" + std::to_wstring(s) + L"秒";
}

// ============================================================
// 构造 / 析构
// ============================================================

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    ui_alive_->store(false, std::memory_order_release);   // S5.5：分离线程止步
    quitting_.store(true);
    if (scanner_) scanner_->cancel();
    if (worker_thread_.joinable()) worker_thread_.join();
    if (hAccel_) DestroyAcceleratorTable(hAccel_);
    if (hFont_) DeleteObject(hFont_);
}

int MainWindow::scale_dpi(int v) const {
    return MulDiv(v, dpi_, 96);
}

// ============================================================
// 创建窗口
// ============================================================

HWND MainWindow::create(HINSTANCE hInst, int nCmdShow) {
    hInst_ = hInst;

    // 获取系统 DPI（R12）
    HDC dc = GetDC(nullptr);
    if (dc) {
        dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
        if (dpi_ <= 0) dpi_ = 96;
        ReleaseDC(nullptr, dc);
    }

    // 注册窗口类
    static const wchar_t* kClassName = L"TextSearchMainWnd";
    WNDCLASSW wc{};
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    // 加载配置确定初始窗口尺寸（F.1.5）
    config_ = load_config();
    int x = config_.window_x ? config_.window_x : CW_USEDEFAULT;
    int y = config_.window_y ? config_.window_y : CW_USEDEFAULT;
    int w = config_.window_w ? config_.window_w : 1200;
    int h = config_.window_h ? config_.window_h : 800;

    hwnd_ = CreateWindowExW(
        0, kClassName, i18n::tr("app.title"),
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        nullptr, nullptr, hInst, this);

    if (!hwnd_) return nullptr;

    // 沉浸式深色标题栏（DWM）：与浅色客户区形成原生高级感对比。
    // 如需跟随系统主题，改为读取注册表 AppsUseLightTheme 后再设 TRUE/FALSE 即可。
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
    {
        BOOL use_dark = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &use_dark, sizeof(use_dark));
    }

    ShowWindow(hwnd_, config_.window_maximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd_);
    return hwnd_;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hWnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }
    if (self) {
        return self->dispatch(msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================
// 消息分发
// ============================================================

LRESULT MainWindow::dispatch(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            on_create();
            return 0;

        case WM_SIZE:
            layout_controls();
            return 0;

        // ---- 分隔条拖拽 ----
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int sp = hit_test_splitter(x, y);
            if (sp > 0) {
                start_drag_splitter(sp, x);
                SetCapture(hwnd_);
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        case WM_MOUSEMOVE: {
            if (dragging_splitter_ > 0) {
                int x = GET_X_LPARAM(lParam);
                on_drag_splitter(x);
                return 0;
            }
            // 非拖拽时不处理 MOUSEMOVE（性能考虑），光标由 WM_SETCURSOR 处理
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        case WM_LBUTTONUP: {
            if (dragging_splitter_ > 0) {
                end_drag_splitter();
                ReleaseCapture();
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        case WM_SETCURSOR: {
            // wParam = 鼠标所在窗口句柄, lParam 低位 = hit-test code
            // 只处理主窗口客户区的 WM_SETCURSOR（子控件自行处理）
            if (reinterpret_cast<HWND>(wParam) == hwnd_ &&
                LOWORD(lParam) == HTCLIENT) {
                if (dragging_splitter_ > 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
                // 悬停检测（hit_test_splitter 内部有空指针/未初始化防御）
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd_, &pt);
                if (hit_test_splitter(pt.x, pt.y) > 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            // 不命中分隔条 → 交给默认处理
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = scale_dpi(800);
            mmi->ptMinTrackSize.y = scale_dpi(500);
            return 0;
        }

        case WM_MENUCHAR:
            // 抑制 Alt+数字 快捷键的系统菜单蜂鸣
            return MAKELRESULT(0, MNC_CLOSE);

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            // S5：历史关键词菜单项 → 回填关键词框并全选（方便直接改）
            if (id >= kHistoryCmdBase &&
                id < kHistoryCmdBase + static_cast<WORD>(kMaxKeywordHistory)) {
                size_t idx = static_cast<size_t>(id - kHistoryCmdBase);
                if (idx < keyword_history_.size()) {
                    SetWindowTextW(hKeywordEdit_, keyword_history_[idx].c_str());
                    SetFocus(hKeywordEdit_);
                    SendMessageW(hKeywordEdit_, EM_SETSEL, 0, -1);
                }
                return 0;
            }
            switch (id) {
                case IDC_BROWSE_BTN:    on_browse();            return 0;
                case IDC_SEARCH_BTN:    on_search();            return 0;
                case IDC_CANCEL_BTN:    on_cancel();            return 0;
                case IDC_COLLECT_BTN:   on_collect_extensions();return 0;
                case IDC_EXPORT_BTN:    on_export_results();    return 0;
                case IDC_INDEX_BTN:     on_build_index();       return 0;
                case IDC_OPT_TOGGLE:    toggle_options();       return 0;
                case CMD_FOCUS_PATH:    SetFocus(hPathEdit_);   return 0;
                case CMD_FOCUS_KEYWORD: SetFocus(hKeywordEdit_);return 0;
                case CMD_EXPORT:        on_export_results();    return 0;
                case CMD_CANCEL:        on_cancel();            return 0;
                case CMD_FOCUS_LEFT:    focus_column(1);        return 0;
                case CMD_FOCUS_MIDDLE:  focus_column(2);        return 0;
                case CMD_FOCUS_RIGHT:   focus_column(3);        return 0;
                case IDC_HISTORY_BTN:   show_keyword_history(); return 0;
                // S7：结果列表右键菜单命令
                case CTX_PREVIEW:       on_result_activate();   return 0;
                case CTX_OPEN_FOLDER:   open_containing_folder(); return 0;
                case CTX_OPEN_EDIT:     open_in_editor();         return 0;
                case CTX_COPY_PATH:     copy_result_field(0);   return 0;
                case CTX_COPY_LINENO:   copy_result_field(1);   return 0;
                case CTX_COPY_LINE:     copy_selected_result(); return 0;
                case IDC_WRAP_CHK: {    // 预览自动换行：纯显示折行，行号始终按文件真实行
                    bool on = (Button_GetCheck(hWrapChk_) == BST_CHECKED);
                    viewer_.set_word_wrap(on);
                    config_.preview_word_wrap = on;   // 退出时随配置落盘
                    return 0;
                }
                case IDC_LANG_COMBO:
                    if (code == CBN_SELCHANGE) {   // 语言切换：落盘 + 重启生效提示
                        int sel = (int)SendMessageW(hLangCombo_, CB_GETCURSEL, 0, 0);
                        config_.ui_lang = (sel == 1) ? L"zh"
                                        : (sel == 2) ? L"en" : L"system";
                        save_settings();
                        MessageBoxW(hwnd_, i18n::tr("msg.lang_restart"),
                                    i18n::tr("msg.lang_restart_t"),
                                    MB_OK | MB_ICONINFORMATION);
                    }
                    return 0;
                default: break;
            }
            // 关键词框内容变化 → 搜索按钮启用/禁用（F.1.4 空 pattern 禁用）
            if (id == IDC_KEYWORD_EDIT && code == EN_CHANGE) {
                std::wstring kw = trim(get_window_text(hKeywordEdit_));
                EnableWindow(hSearchBtn_, !kw.empty() && state_ == State::Idle);
            }
            // 路径框失焦 → 刷新「索引：已建立/未建立」状态（手动输入/粘贴路径的时机）
            if (id == IDC_PATH_EDIT && code == EN_KILLFOCUS) {
                update_index_status();
            }
            return 0;
        }

        case WM_NOTIFY: {
            auto* nmh = reinterpret_cast<LPNMHDR>(lParam);
            if (nmh->idFrom == IDC_RESULT_LIST) {
                switch (nmh->code) {
                    case NM_CUSTOMDRAW:
                        return list_view_custom_draw(lParam);
                    case LVN_GETDISPINFO: {
                        auto* plvdi = reinterpret_cast<NMLVDISPINFOW*>(lParam);
                        int idx = plvdi->item.iItem;
                        if (idx < 0 || idx >= static_cast<int>(results_.size())) break;
                        const auto& r = results_[static_cast<size_t>(idx)];
                        if (plvdi->item.mask & LVIF_TEXT) {
                            switch (plvdi->item.iSubItem) {
                                case 0: {  // 文件路径
                                    std::wstring p = r.file_path.wstring();
                                    wcsncpy_s(plvdi->item.pszText,
                                              plvdi->item.cchTextMax,
                                              p.c_str(), _TRUNCATE);
                                    break;
                                }
                                case 1: {  // 行号：命中行/文件总行（总行懒计算）
                                    int total = file_line_total(r.file_path);
                                    std::wstring s = std::to_wstring(r.line_no);
                                    if (total > 0)
                                        s += L"/" + std::to_wstring(total);
                                    wcsncpy_s(plvdi->item.pszText,
                                              plvdi->item.cchTextMax,
                                              s.c_str(), _TRUNCATE);
                                    break;
                                }
                                case 2: {  // 命中行文本
                                    wcsncpy_s(plvdi->item.pszText,
                                              plvdi->item.cchTextMax,
                                              r.line_text.c_str(), _TRUNCATE);
                                    break;
                                }
                            }
                        }
                        return 0;
                    }
                    case NM_DBLCLK:
                        on_result_activate();
                        return 0;
                    case LVN_KEYDOWN: {
                        auto* lvkd = reinterpret_cast<LPNMLVKEYDOWN>(lParam);
                        if (lvkd->wVKey == VK_RETURN) {
                            on_result_activate();
                            return 0;
                        }
                        if (lvkd->wVKey == 'C' &&
                            (GetKeyState(VK_CONTROL) & 0x8000)) {
                            copy_selected_result();
                            return 0;
                        }
                        break;
                    }
                    case LVN_COLUMNCLICK: {   // S1：点击列头排序
                        auto* plvn = reinterpret_cast<LPNMLISTVIEW>(lParam);
                        if (plvn->iSubItem == sort_col_) {
                            sort_asc_ = !sort_asc_;          // 同列再点 → 反向
                        } else {
                            sort_col_ = plvn->iSubItem;       // 换列 → 默认升序
                            sort_asc_ = true;
                        }
                        apply_sort();
                        ListView_RedrawItems(hResultList_, 0,
                                            static_cast<int>(results_.size()) - 1);
                        return 0;
                    }
                }
            } else if (nmh->idFrom == IDC_EXT_LIST) {
                if (nmh->code == NM_CUSTOMDRAW)
                    return list_view_custom_draw(lParam);
            }
            return 0;
        }

        case WM_CONTEXTMENU: {   // S7：结果列表右键菜单
            if (reinterpret_cast<HWND>(wParam) != hResultList_) break;
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int idx = -1;
            if (pt.x == -1 && pt.y == -1) {
                // 键盘触发（Shift+F10 / ▤菜单键）：定位到当前选中项下方
                idx = selected_result_index();
                RECT rc{};
                if (idx >= 0 &&
                    ListView_GetItemRect(hResultList_, idx, &rc, LVIR_LABEL)) {
                    pt = { rc.left, rc.bottom };
                    ClientToScreen(hResultList_, &pt);
                } else {
                    // 无选中项：菜单落在列表中央，而不是 (-1,-1) 的屏幕左上角
                    RECT lrc{};
                    GetClientRect(hResultList_, &lrc);
                    pt = { (lrc.left + lrc.right) / 2,
                           (lrc.top + lrc.bottom) / 2 };
                    ClientToScreen(hResultList_, &pt);
                }
            } else {
                POINT cpt = pt;
                ScreenToClient(hResultList_, &cpt);
                LVHITTESTINFO hti{};
                hti.pt = cpt;
                int hit = static_cast<int>(ListView_HitTest(hResultList_, &hti));
                if (hit >= 0) {
                    idx = hit;
                    // 右键即选中，让菜单命令作用于被点击的那一行
                    ListView_SetItemState(hResultList_, idx,
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            show_result_context_menu(pt, idx >= 0);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            // 焦点在主窗口自身或不处理滚轮的子控件（编辑框等）经 DefWindowProc
            // 冒泡到这里：光标在预览区 → 转发（列表场景由 list_wheel_proc 处理）
            DWORD pos = GetMessagePos();
            POINT pt{ GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
            ScreenToClient(hwnd_, &pt);
            if (viewer_.contains_point(pt)) {
                return SendMessageW(viewer_.hwnd(), WM_MOUSEWHEEL,
                                    wParam, lParam);
            }
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
        }

        case WM_DROPFILES: {   // O1：拖拽目录/文件到窗口 → 填入路径框
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            if (hDrop) {
                wchar_t buf[MAX_PATH];
                // 只取第一个拖入项（拖一堆目录也没意义，取首个最合理）
                if (DragQueryFileW(hDrop, 0, buf, MAX_PATH) > 0) {
                    std::filesystem::path p(buf);
                    std::error_code ec;
                    // 拖的是文件 → 取所在目录；拖的是目录 → 直接用
                    if (std::filesystem::is_regular_file(p, ec))
                        p = p.parent_path();
                    if (std::filesystem::is_directory(p, ec)) {
                        SetWindowTextW(hPathEdit_, p.wstring().c_str());
                        update_index_status();   // 触发索引状态检测
                        set_status(L"已填入拖入的目录：" + p.wstring());
                    } else {
                        set_status(L"拖入的不是有效目录：" + p.wstring());
                    }
                }
                DragFinish(hDrop);
            }
            return 0;
        }

        case WM_PROGRESS:
            on_progress(static_cast<size_t>(wParam), static_cast<size_t>(lParam));
            return 0;

        case WM_RESULTS:
            on_results(reinterpret_cast<std::vector<MatchResult>*>(lParam));
            return 0;

        case WM_SKIP:
            on_skip(static_cast<size_t>(wParam));
            return 0;

        case WM_INDEX_STATUS: {
            // 异步索引检测回包：只在请求的代际号仍是当前最新时刷新（旧的被忽略）
            uint64_t gen = static_cast<uint64_t>(wParam);
            if (gen == index_check_gen_.load(std::memory_order_acquire)) {
                const wchar_t* text = (lParam != 0) ? i18n::tr("sb.index_ok")
                                            : i18n::tr("sb.index_no");
                SendMessageW(hStatusBar_, SB_SETTEXTW,
                             MAKEWPARAM(2, 0), (LPARAM)text);
            }
            return 0;
        }

        case WM_SEARCH_MODE: {
            // worker 线程预判本次搜索实际模式后回传。UI 线程据此：
            //   1) 写 search_mode_label_（后续 on_results/on_done 文本会用）
            //   2) 若仍在扫描中，把"正在扫描…"补上模式
            //   3) 同步写索引 pane（mode 判定即最新事实，作废飞行中的旧异步检测）
            bool use_index = (lParam != 0);
            search_mode_label_ = use_index ? L"（索引模式）" : L"（实时扫描）";
            index_mode_ = use_index;   // S6：状态栏文件计数据此切换文案
            index_check_gen_.fetch_add(1, std::memory_order_acq_rel);
            SendMessageW(hStatusBar_, SB_SETTEXTW, MAKEWPARAM(2, 0),
                         (LPARAM)(use_index ? i18n::tr("sb.index_ok")
                                            : i18n::tr("sb.index_no")));
            if (state_ == State::Scanning) {
                set_status(L"正在扫描…" + search_mode_label_);
            }
            return 0;
        }

        case WM_DONE:
            on_done(static_cast<int>(wParam),
                    reinterpret_cast<std::wstring*>(lParam));
            return 0;

        case WM_TIMER:
            if (wParam == kWatchdogTimerId) on_watchdog();
            return 0;

        case WM_DPICHANGED: {
            // main.cpp 声明了 Per-Monitor V2 DPI 感知，但窗口若不响应
            // WM_DPICHANGED，跨不同 DPI 显示器拖动后：布局常量仍按旧 DPI
            // 计算、字体不重建、记忆的列宽错位——整个界面比例失衡。
            int new_dpi = static_cast<int>(HIWORD(wParam));
            if (new_dpi > 0 && new_dpi != dpi_ && hResultList_) {
                double ratio = static_cast<double>(new_dpi) /
                               static_cast<double>(dpi_);
                dpi_ = new_dpi;

                // 重建全局字体并广播给全部子控件（含查看器 RichEdit）
                if (hFont_) DeleteObject(hFont_);
                hFont_ = CreateFontW(-MulDiv(12, dpi_, 96), 0, 0, 0,
                                     FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS,
                                     L"Microsoft YaHei UI");
                if (hFont_) {
                    EnumChildWindows(hwnd_,
                        [](HWND h, LPARAM lp) -> BOOL {
                            SendMessageW(h, WM_SETFONT,
                                         static_cast<WPARAM>(lp), TRUE);
                            return TRUE;
                        },
                        reinterpret_cast<LPARAM>(hFont_));
                }

                // 记忆的三栏列宽与结果列宽按 DPI 比例换算（存的是像素值）
                left_col_w_ = static_cast<int>(left_col_w_ * ratio);
                right_col_w_ = static_cast<int>(right_col_w_ * ratio);
                for (int i = 0; i < 3; ++i) {
                    int w = ListView_GetColumnWidth(hResultList_, i);
                    ListView_SetColumnWidth(
                        hResultList_, i, static_cast<int>(w * ratio));
                }

                // 按系统建议矩形缩放窗口本体，再重排
                auto* pr = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd_, nullptr, pr->left, pr->top,
                             pr->right - pr->left, pr->bottom - pr->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                layout_controls();
            }
            return 0;
        }

        case WM_CLOSE:
            // B4：建库一旦开始无法中断（Indexer::build 无取消点，中断写索引
            // 风险大于收益）。关窗前明确告知「将等待建库完成」，避免大目录
            // 建库期间窗口看似未响应而被强杀（索引写一半虽可被 checksum 检出
            // 并自动重建，但结果正确≠体验可接受）。
            if (state_ == State::Scanning && !scanner_ && !quitting_.load()) {
                int r = MessageBoxW(hwnd_,
                    i18n::tr("close_build.msg"),
                    i18n::tr("close_build.title"), MB_YESNO | MB_ICONWARNING);
                if (r != IDYES) return 0;
            }
            // F.2.9：销毁前先取消并等待 worker 退出
            ui_alive_->store(false, std::memory_order_release);   // S5.5
            quitting_.store(true);
            if (scanner_) scanner_->cancel();
            if (worker_thread_.joinable()) worker_thread_.join();
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY: {
            if (timer_id_) { KillTimer(hwnd_, timer_id_); timer_id_ = 0; }
            // 排空残留的自定义消息，释放指针（F.2.9 防泄漏）
            MSG msg;
            while (PeekMessageW(&msg, hwnd_, WM_APP + 1, WM_APP + 4, PM_REMOVE)) {
                if (msg.message == WM_RESULTS) {
                    delete reinterpret_cast<std::vector<MatchResult>*>(msg.lParam);
                } else if (msg.message == WM_DONE && msg.lParam) {
                    delete reinterpret_cast<std::wstring*>(msg.lParam);
                }
            }
            save_settings();
            logger::info(L"TextSearch 退出");
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
    // 兜底：不应到达（所有 case 都有 return），但防止 UB
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

// ============================================================
// 控件创建
// ============================================================

void MainWindow::on_create() {
    // 初始化 Common Controls（ListView / ProgressBar）
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    create_controls();
    create_accelerators();
    // O1：允许把目录/文件拖进窗口直接填入路径（省去「浏览」点击）
    DragAcceptFiles(hwnd_, TRUE);
    // 可配置常量注入（ini [Preview]）
    viewer_.set_large_file_limits(
        static_cast<size_t>(config_.preview_large_file_mb) * 1024 * 1024,
        config_.preview_large_file_ctx);
    viewer_.set_max_highlights(
        static_cast<size_t>(config_.preview_max_highlights));
    viewer_.set_context_lines(config_.preview_context_lines);

    load_settings();
    update_index_status();   // 恢复上次路径后即显示索引状态
    set_state(State::Idle);

    // B2：日志隐私开关先于任何日志生效（load_settings 已把配置读进 config_）
    logger::init();
    logger::set_enabled(config_.log_enabled);
    logger::set_redact_paths(config_.log_redact_paths);
    logger::info(L"TextSearch 启动");
}

void MainWindow::create_controls() {
    // 微软雅黑 12pt（ClearType 渲染），2K 屏幕下与窗口标题栏字号匹配
    int font_h = -MulDiv(12, dpi_, 96);
    hFont_ = CreateFontW(font_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                         L"Microsoft YaHei UI");
    HFONT hFont = hFont_ ? hFont_
                         : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    auto create_btn = [&](int id, const wchar_t* text) -> HWND {
        HWND hw = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(id), hInst_, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        return hw;
    };

    auto create_edit = [&](int id, DWORD extra_style) -> HWND {
        HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra_style,
            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(id), hInst_, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        SetWindowSubclass(hw, &CenteredEditProc, 0, 0);   // 编辑框垂直居中
        return hw;
    };

    auto create_chk = [&](int id, const wchar_t* text) -> HWND {
        HWND hw = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(id), hInst_, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        return hw;
    };

    auto create_label = [&](const wchar_t* text) -> HWND {
        HWND hw = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFT,
            0, 0, 10, 10, hwnd_, nullptr, hInst_, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        return hw;
    };

    // ---- 顶部输入条 ----
    hLblPath_ = create_label(i18n::tr("label.path"));
    hPathEdit_ = create_edit(IDC_PATH_EDIT, 0);
    SendMessageW(hPathEdit_, EM_SETCUEBANNER, FALSE,
                 (LPARAM)i18n::tr("cue.path"));
    hBrowseBtn_ = create_btn(IDC_BROWSE_BTN, i18n::tr("btn.browse"));
    hLblKeyword_ = create_label(i18n::tr("label.keyword"));
    hKeywordEdit_ = create_edit(IDC_KEYWORD_EDIT, 0);
    SendMessageW(hKeywordEdit_, EM_SETCUEBANNER, FALSE,
                 (LPARAM)i18n::tr("cue.keyword"));
    // S5：紧邻关键词框的「历史」下拉按钮
    hHistoryBtn_ = create_btn(IDC_HISTORY_BTN, L"▾");
    hSearchBtn_ = create_btn(IDC_SEARCH_BTN, i18n::tr("btn.search"));
    hCancelBtn_ = create_btn(IDC_CANCEL_BTN, i18n::tr("btn.cancel"));
    hCollectBtn_ = create_btn(IDC_COLLECT_BTN, i18n::tr("btn.collect"));
    hExportBtn_ = create_btn(IDC_EXPORT_BTN, i18n::tr("btn.export"));
    hIndexBtn_ = create_btn(IDC_INDEX_BTN, i18n::tr("btn.index"));

    // 顶部输入条分组用的竖直分隔线（淡蚀刻线，仅起视觉分组作用，非装饰）
    auto create_sep = [&]() -> HWND {
        return CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ETCHEDVERT,
            0, 0, 2, 2, hwnd_, nullptr, hInst_, nullptr);
    };
    hSep1_ = create_sep();
    hSep2_ = create_sep();

    // 关键词框子类化：Enter 触发搜索（id=1，与 id=0 的居中子类共存于子类链）
    SetWindowSubclass(hKeywordEdit_, &KeywordEditProc, 1,
                      reinterpret_cast<DWORD_PTR>(this));

    // ---- 选项条 ----
    hOptToggle_ = create_btn(IDC_OPT_TOGGLE, i18n::tr("opt.open"));
    hChkRecursive_ = create_chk(IDC_CHK_RECURSIVE, i18n::tr("chk.recursive"));
    hChkRegex_ = create_chk(IDC_CHK_REGEX, i18n::tr("chk.regex"));
    hChkCase_ = create_chk(IDC_CHK_CASE, i18n::tr("chk.case"));
    hChkWholeWord_ = create_chk(IDC_CHK_WHOLEWORD, i18n::tr("chk.wholeword"));
    hChkRealtime_ = create_chk(IDC_CHK_REALTIME, i18n::tr("chk.realtime"));
    hLblThreshold_ = create_label(i18n::tr("lbl.threshold"));
    hThresholdEdit_ = create_edit(IDC_THRESHOLD_EDIT, ES_NUMBER);
    hLblThresholdMb_ = create_label(L"MB");
    hLblWildcard_ = create_label(i18n::tr("lbl.wildcard"));
    hWildcardEdit_ = create_edit(IDC_WILDCARD_EDIT, 0);
    hLblBlacklist_ = create_label(i18n::tr("lbl.blacklist"));
    hBlacklistEdit_ = create_edit(IDC_BLACKLIST_EDIT, 0);
    // 语言选择（跟随系统/简体中文/English；切换后重启生效）
    hLblLang_ = create_label(i18n::tr("lbl.lang"));
    hLangCombo_ = CreateWindowExW(0, L"combobox", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_LANG_COMBO),
        hInst_, nullptr);
    SendMessageW(hLangCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    SendMessageW(hLangCombo_, CB_ADDSTRING, 0, (LPARAM)i18n::tr("lang.system"));
    SendMessageW(hLangCombo_, CB_ADDSTRING, 0, (LPARAM)i18n::tr("lang.zh"));
    SendMessageW(hLangCombo_, CB_ADDSTRING, 0, (LPARAM)i18n::tr("lang.en"));

    // 默认值
    SetWindowTextW(hThresholdEdit_, L"50");
    Button_SetCheck(hChkRecursive_, BST_CHECKED);

    // ---- 左栏：扩展名列表 ----
    hExtLabel_ = create_label(i18n::tr("label.ext"));
    hExtList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_EX_CHECKBOXES |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_VSCROLL | WS_HSCROLL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_EXT_LIST),
        hInst_, nullptr);
    SendMessageW(hExtList_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    ListView_SetExtendedListViewStyle(hExtList_,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    // 悬停滚轮转发：光标在预览区上时滚轮给预览（焦点仍在列表，↑/↓ 不受影响）
    SetWindowSubclass(hExtList_, &list_wheel_proc, 10,
                      reinterpret_cast<DWORD_PTR>(this));
    {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 200;
        const wchar_t* coltext = i18n::tr("col.ext");
        col.pszText = const_cast<LPWSTR>(coltext);
        ListView_InsertColumn(hExtList_, 0, &col);
    }

    // ---- 中栏：结果虚拟列表（R13）----
    hResultLabel_ = create_label(i18n::tr("label.result"));
    hResultList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | WS_VSCROLL | WS_HSCROLL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_RESULT_LIST),
        hInst_, nullptr);
    SendMessageW(hResultList_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    ListView_SetExtendedListViewStyle(hResultList_,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    // 悬停滚轮转发：双击后焦点留在列表，滚轮悬停在预览上必须能滚预览
    SetWindowSubclass(hResultList_, &list_wheel_proc, 11,
                      reinterpret_cast<DWORD_PTR>(this));
    {
        struct Col { const wchar_t* name; int cx; };
        Col cols[] = {
            {i18n::tr("col.path"), 300},
            {i18n::tr("col.line"), 60},
            {i18n::tr("col.hit"), 400},
        };
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
            col.iSubItem = i;
            col.cx = scale_dpi(cols[i].cx);
            col.fmt = (i == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;  // 行号列右对齐
            col.pszText = const_cast<LPWSTR>(cols[i].name);
            ListView_InsertColumn(hResultList_, i, &col);
        }
        ListView_SetColumnWidth(hResultList_, 1, lineno_col_fixed_width());
    }

    // ---- 右栏：查看器标题 + 自动换行开关 + RichEdit ----
    hViewerLabel_ = create_label(i18n::tr("label.preview"));
    hWrapChk_ = create_chk(IDC_WRAP_CHK, i18n::tr("chk.wrap"));
    viewer_.create(hwnd_, hInst_);
    // 查看器与全界面同字体：Viewer 内部只挂了 DEFAULT_GUI_FONT（8pt 系统
    // 字体），比雅黑 12pt 小一号且风格突兀，DPI 高时更明显。
    if (hFont) SendMessageW(viewer_.hwnd(), WM_SETFONT,
                            reinterpret_cast<WPARAM>(hFont), TRUE);
    if (hWrapChk_) Button_SetCheck(hWrapChk_, BST_CHECKED);   // 默认开，load_settings 再按配置纠正

    // ---- 底部状态栏 + 进度条 ----
    hStatusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(IDC_STATUSBAR),
        hInst_, nullptr);
    SendMessageW(hStatusBar_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

    hProgressBar_ = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hStatusBar_, reinterpret_cast<HMENU>(IDC_PROGRESS),
        hInst_, nullptr);
}

// ============================================================
// 布局
// ============================================================

void MainWindow::layout_controls() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    const int margin = scale_dpi(8);      // 外边距
    const int gap = scale_dpi(6);         // 组内间距
    const int group_gap = scale_dpi(18);  // 组间间距（视觉分组）
    const int edit_h = scale_dpi(30);
    const int btn_h = scale_dpi(32);
    const int input_bar_h = scale_dpi(42);
    const int opt_bar_h_expanded = scale_dpi(36);
    const int opt_bar_h_collapsed = scale_dpi(28);
    int opt_bar_h = options_expanded_ ? opt_bar_h_expanded : opt_bar_h_collapsed;

    // ---- 底部状态栏 ----
    SendMessageW(hStatusBar_, WM_SIZE, 0, 0);
    RECT sb_rc;
    GetClientRect(hStatusBar_, &sb_rc);
    int status_h = sb_rc.bottom - sb_rc.top;

    // 状态栏分四部分：part0=进度条，part1=文件计数，part2=索引状态，part3=状态文本
    int prog_w = scale_dpi(160);
    int count_w = scale_dpi(140);
    int idx_w = scale_dpi(150);
    int edge = scale_dpi(2);
    int parts[4];
    parts[0] = prog_w;
    parts[1] = prog_w + count_w;
    parts[2] = prog_w + count_w + idx_w;
    parts[3] = -1;
    SendMessageW(hStatusBar_, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
    // 进度条放在 part0
    RECT part0;
    SendMessageW(hStatusBar_, SB_GETRECT, 0, reinterpret_cast<LPARAM>(&part0));
    MapWindowPoints(hStatusBar_, hwnd_, reinterpret_cast<LPPOINT>(&part0), 2);
    MoveWindow(hProgressBar_,
               part0.left + edge, part0.top + edge,
               (part0.right - part0.left) - edge * 2,
               (part0.bottom - part0.top) - edge * 2, TRUE);

    // ---- 顶部输入条（按功能分 3 组：搜索位置 / 搜索内容 / 数据操作，组间用竖直分隔线）----
    int y = 0;
    int x = margin;
    int h = input_bar_h;
    auto row_y = [y, h](int ch) { return y + (h - ch) / 2; };
    // 编辑框垂直居中已由 CenteredEditProc 子类化在 WM_NCCALCSIZE 完成，
    // 这里用纯几何居中即可，不再手动加 Y 偏移（旧 edit_v_off hack 已移除）
    auto edit_y = [y, h, edit_h]() { return y + (h - edit_h) / 2; };

    // 组1：搜索位置（路径 + 浏览）。
    // 控件宽度按自身文本实测（GetTextExtent），中英文/DPI 通用——英文按钮
    // 文案比中文长，固定宽度会截断（「ollect Extension」即此）。
    auto text_w = [&](HWND hw, int pad) -> int {
        HDC tdc = GetDC(hw);
        if (!tdc) return scale_dpi(60);
        HFONT f = reinterpret_cast<HFONT>(
            SendMessageW(hw, WM_GETFONT, 0, 0));
        HGDIOBJ oldf = f ? SelectObject(tdc, f) : nullptr;
        wchar_t buf[160];
        int tl = GetWindowTextW(hw, buf, 160);
        SIZE sz{};
        BOOL okf = (tl > 0) ? GetTextExtentPoint32W(tdc, buf, tl, &sz) : FALSE;
        if (oldf) SelectObject(tdc, oldf);
        ReleaseDC(hwnd_, tdc);
        return okf ? sz.cx + pad : scale_dpi(60);
    };

    int lbl_w = text_w(hLblPath_, scale_dpi(4));
    MoveWindow(hLblPath_, x, row_y(edit_h), lbl_w, edit_h, TRUE);
    x += lbl_w + gap;
    int path_w = scale_dpi(340);
    MoveWindow(hPathEdit_, x, edit_y(), path_w, edit_h, TRUE);
    x += path_w + gap;
    int browse_w = text_w(hBrowseBtn_, scale_dpi(20));
    MoveWindow(hBrowseBtn_, x, row_y(btn_h), browse_w, btn_h, TRUE);
    x += browse_w + group_gap;
    // 分隔线 G1|G2
    MoveWindow(hSep1_, x - group_gap / 2 - scale_dpi(1), y + scale_dpi(6),
               scale_dpi(2), input_bar_h - scale_dpi(12), TRUE);

    // 组2：搜索内容（关键词 + 搜索 / 取消）
    int kw_lbl_w = text_w(hLblKeyword_, scale_dpi(4));
    MoveWindow(hLblKeyword_, x, row_y(edit_h), kw_lbl_w, edit_h, TRUE);
    x += kw_lbl_w + gap;
    int kw_w = scale_dpi(184);          // S5：让出历史按钮的宽度（+26+gap）
    MoveWindow(hKeywordEdit_, x, edit_y(), kw_w, edit_h, TRUE);
    x += kw_w + gap;
    int hist_w = scale_dpi(26);         // S5：关键词历史下拉
    MoveWindow(hHistoryBtn_, x, row_y(btn_h), hist_w, btn_h, TRUE);
    x += hist_w + gap;
    int act_w = text_w(hSearchBtn_, scale_dpi(20));
    MoveWindow(hSearchBtn_, x, row_y(btn_h), act_w, btn_h, TRUE);
    x += act_w + gap;
    int cancel_w = text_w(hCancelBtn_, scale_dpi(20));
    MoveWindow(hCancelBtn_, x, row_y(btn_h), cancel_w, btn_h, TRUE);
    x += cancel_w + group_gap;
    // 分隔线 G2|G3
    MoveWindow(hSep2_, x - group_gap / 2 - scale_dpi(1), y + scale_dpi(6),
               scale_dpi(2), input_bar_h - scale_dpi(12), TRUE);

    // 组3：数据操作（收集扩展名 / 导出结果 / 建立索引）
    int collect_w = text_w(hCollectBtn_, scale_dpi(22));
    MoveWindow(hCollectBtn_, x, row_y(btn_h), collect_w, btn_h, TRUE);
    x += collect_w + gap;
    int exp_w = text_w(hExportBtn_, scale_dpi(22));
    MoveWindow(hExportBtn_, x, row_y(btn_h), exp_w, btn_h, TRUE);
    x += exp_w + gap;
    int index_w = text_w(hIndexBtn_, scale_dpi(22));
    MoveWindow(hIndexBtn_, x, row_y(btn_h), index_w, btn_h, TRUE);

    y += input_bar_h;

    // ---- 选项条（布尔开关组 + 取值组，标签紧贴对应编辑框）----
    int toggle_w = text_w(hOptToggle_, scale_dpi(16));
    MoveWindow(hOptToggle_, margin, y + (opt_bar_h - btn_h) / 2,
               toggle_w, btn_h, TRUE);
    int ox = margin + toggle_w + group_gap;
    // 选项条内编辑框：居中同样交由 CenteredEditProc，这里纯几何居中即可
    int opt_ey = y + (opt_bar_h - edit_h) / 2;
    // 复选框宽度 = 文本 + 框体；标签宽度 = 文本 + 边距
    auto place_chk = [&](HWND hw) {
        int w = text_w(hw, scale_dpi(24));
        MoveWindow(hw, ox, opt_ey, w, edit_h, TRUE);
        ox += w + gap;
    };
    auto place_field = [&](HWND lbl, HWND ed, int ew) {
        int lw = text_w(lbl, scale_dpi(4));
        MoveWindow(lbl, ox, y + (opt_bar_h - edit_h) / 2, lw, edit_h, TRUE);
        ox += lw;
        MoveWindow(ed, ox, opt_ey, ew, edit_h, TRUE);
        ox += ew + group_gap;
    };

    if (options_expanded_) {
        // 布尔开关组（宽度按文本实测，en/zh/DPI 通用）
        place_chk(hChkRecursive_);
        place_chk(hChkRegex_);
        place_chk(hChkCase_);
        place_chk(hChkWholeWord_);
        place_chk(hChkRealtime_);
        ox += group_gap;  // 与取值组分隔
        // 取值组：标签紧贴编辑框；MB 单位紧跟阈值编辑框
        place_field(hLblThreshold_, hThresholdEdit_, scale_dpi(40));
        MoveWindow(hLblThresholdMb_, ox, y + (opt_bar_h - edit_h) / 2,
                   scale_dpi(24), edit_h, TRUE);
        ox += scale_dpi(24) + group_gap;
        place_field(hLblWildcard_, hWildcardEdit_, scale_dpi(120));
        place_field(hLblBlacklist_, hBlacklistEdit_, scale_dpi(120));
        place_field(hLblLang_, hLangCombo_, scale_dpi(104));
    } else {
        ShowWindow(hLblLang_, SW_HIDE);
        ShowWindow(hLangCombo_, SW_HIDE);
        // 折叠：隐藏选项控件
        auto hide = [](HWND hw) { ShowWindow(hw, SW_HIDE); };
        hide(hChkRecursive_); hide(hChkRegex_); hide(hChkCase_);
        hide(hChkWholeWord_); hide(hChkRealtime_);
        hide(hLblThreshold_); hide(hThresholdEdit_);
        hide(hLblThresholdMb_); hide(hLblWildcard_); hide(hWildcardEdit_);
        hide(hLblBlacklist_); hide(hBlacklistEdit_);
        hide(hLblLang_); hide(hLangCombo_);
    }
    if (options_expanded_) {
        auto show = [](HWND hw) { ShowWindow(hw, SW_SHOWNOACTIVATE); };
        show(hChkRecursive_); show(hChkRegex_); show(hChkCase_);
        show(hChkWholeWord_); show(hChkRealtime_);
        show(hLblThreshold_); show(hThresholdEdit_);
        show(hLblThresholdMb_); show(hLblWildcard_); show(hWildcardEdit_);
        show(hLblBlacklist_); show(hBlacklistEdit_);
        show(hLblLang_); show(hLangCombo_);
    }

    y += opt_bar_h;

    // ---- 三栏 ----
    int col_top = scale_dpi(6);    // 三栏区域与上方选项条的留白
    int col_area_h = H - y - status_h - col_top;
    if (col_area_h < scale_dpi(100)) col_area_h = scale_dpi(100);

    // 分隔条宽度
    splitter_w_ = scale_dpi(4);

    // 初始化默认列宽（从配置或默认值）
    int default_left = scale_dpi(220);
    int default_right = scale_dpi(360);
    if (left_col_w_ <= 0) {
        left_col_w_ = (config_.left_col_w > 0) ? config_.left_col_w : default_left;
    }
    if (right_col_w_ <= 0) {
        right_col_w_ = (config_.right_col_w > 0) ? config_.right_col_w : default_right;
    }

    // 约束最小宽度
    int min_left = scale_dpi(120);
    int min_right = scale_dpi(150);
    int min_middle = scale_dpi(200);

    if (left_col_w_ < min_left) left_col_w_ = min_left;
    if (right_col_w_ < min_right) right_col_w_ = min_right;

    int total_for_cols = W;
    // 确保中栏不小于 min_middle
    if (total_for_cols - left_col_w_ - right_col_w_ - 2 * splitter_w_ < min_middle) {
        // 优先压缩右栏
        right_col_w_ = total_for_cols - left_col_w_ - 2 * splitter_w_ - min_middle;
        if (right_col_w_ < min_right) {
            right_col_w_ = min_right;
            left_col_w_ = total_for_cols - right_col_w_ - 2 * splitter_w_ - min_middle;
            if (left_col_w_ < min_left) left_col_w_ = min_left;
        }
    }

    int left_w = left_col_w_;
    int right_w = right_col_w_;
    int middle_w = W - left_w - right_w - 2 * splitter_w_;
    if (middle_w < min_middle) middle_w = min_middle;

    // 左栏
    int lbl_h = scale_dpi(20);
    MoveWindow(hExtLabel_, margin, y + col_top, left_w - margin * 2, lbl_h, TRUE);
    MoveWindow(hExtList_, margin, y + col_top + lbl_h + 4,
               left_w - margin * 2, col_area_h - lbl_h - 8, TRUE);
    ListView_SetColumnWidth(hExtList_, 0, left_w - margin * 2 - scale_dpi(20));

    // 中栏（标题与左/右栏对齐，形成统一视觉层次）
    int mx = left_w + splitter_w_;
    MoveWindow(hResultLabel_, mx, y + col_top, middle_w - 2, lbl_h, TRUE);
    MoveWindow(hResultList_, mx, y + col_top + lbl_h + 4,
               middle_w - 2, col_area_h - lbl_h - 8, TRUE);

    // 右栏（标题行右侧让出「自动换行」复选框的位置）
    int rx = left_w + splitter_w_ + middle_w + splitter_w_;
    int wrap_w = scale_dpi(96);
    MoveWindow(hViewerLabel_, rx + margin, y + col_top,
               right_w - margin * 2 - wrap_w - gap, lbl_h, TRUE);
    if (hWrapChk_) {
        MoveWindow(hWrapChk_, rx + margin + right_w - margin * 2 - wrap_w,
                   y + col_top - 2, wrap_w, lbl_h + 4, TRUE);
    }
    viewer_.resize(rx + margin, y + col_top + lbl_h + 4,
                   right_w - margin * 2, col_area_h - lbl_h - 8);
}

// ============================================================
// 分隔条拖拽
// ============================================================

int MainWindow::hit_test_splitter(int x, int y) const {
    if (!hwnd_) return 0;
    // 控件尚未创建（WM_SETCURSOR 可能在 on_create 期间被触发）
    if (!hStatusBar_) return 0;
    // 列宽尚未初始化（layout_controls 还没跑过）
    if (left_col_w_ <= 0 || right_col_w_ <= 0 || splitter_w_ <= 0) return 0;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int W = rc.right - rc.left;
    if (W <= 0) return 0;

    // 计算三栏区域起始 y（跳过顶部输入条 + 选项条 + 状态栏）。
    // ⚠️ 必须与 layout_controls 用同一组常量（42/36/28）——此前这里写的是
    // 36/32/24，命中区整体比真实三栏区上移了 10px：选项条空白处会被误判
    // 为拖拽分隔条，而真实分隔条底部反而拖不到。
    const int input_bar_h = scale_dpi(42);
    int opt_bar_h = options_expanded_ ? scale_dpi(36) : scale_dpi(28);
    int col_y = input_bar_h + opt_bar_h;
    RECT sb_rc;
    GetClientRect(hStatusBar_, &sb_rc);
    int status_h = sb_rc.bottom - sb_rc.top;
    int col_h = (rc.bottom - rc.top) - col_y - status_h;
    if (col_h < scale_dpi(100)) col_h = scale_dpi(100);

    // y 不在三栏区域内 → 不命中
    if (y < col_y || y > col_y + col_h) return 0;

    int hit_zone = scale_dpi(6);  // 命中区域宽度（±3px 容差）
    int sp = splitter_w_;

    // 分隔条 1：左/中之间，x 在 left_col_w_ 附近
    int sp1_x = left_col_w_;
    if (x >= sp1_x - hit_zone && x <= sp1_x + sp + hit_zone) return 1;

    // 分隔条 2：中/右之间
    int middle_w = W - left_col_w_ - right_col_w_ - 2 * sp;
    if (middle_w <= 0) return 0;  // 布局异常时不命中
    int sp2_x = left_col_w_ + sp + middle_w;
    if (x >= sp2_x - hit_zone && x <= sp2_x + sp + hit_zone) return 2;

    return 0;
}

void MainWindow::start_drag_splitter(int which, int x) {
    dragging_splitter_ = which;
    drag_anchor_x_ = x;
    drag_anchor_w_ = (which == 1) ? left_col_w_ : right_col_w_;
}

void MainWindow::on_drag_splitter(int x) {
    int dx = x - drag_anchor_x_;
    int min_left = scale_dpi(120);
    int min_right = scale_dpi(150);
    int min_middle = scale_dpi(200);

    if (dragging_splitter_ == 1) {
        // 左/中分隔条：拖动改变 left_col_w_
        int new_left = drag_anchor_w_ + dx;
        if (new_left < min_left) new_left = min_left;

        RECT rc;
        GetClientRect(hwnd_, &rc);
        int W = rc.right - rc.left;
        // 确保中栏不小于 min_middle
        int max_left = W - right_col_w_ - 2 * splitter_w_ - min_middle;
        if (new_left > max_left) new_left = max_left;

        if (new_left != left_col_w_) {
            left_col_w_ = new_left;
            layout_controls();
        }
    } else if (dragging_splitter_ == 2) {
        // 中/右分隔条：拖动改变 right_col_w_
        // 分隔条在中栏右边界，向右拖(dx>0) → 分隔条右移 → 右栏变小
        // 向左拖(dx<0) → 分隔条左移 → 右栏变大
        int new_right = drag_anchor_w_ - dx;
        if (new_right < min_right) new_right = min_right;

        RECT rc;
        GetClientRect(hwnd_, &rc);
        int W = rc.right - rc.left;
        // 确保中栏不小于 min_middle
        int max_right = W - left_col_w_ - 2 * splitter_w_ - min_middle;
        if (new_right > max_right) new_right = max_right;

        if (new_right != right_col_w_) {
            right_col_w_ = new_right;
            layout_controls();
        }
    }
}

void MainWindow::end_drag_splitter() {
    dragging_splitter_ = 0;
    // 持久化列宽
    config_.left_col_w = left_col_w_;
    config_.right_col_w = right_col_w_;
    save_settings();
}

// ============================================================
// 状态机
// ============================================================

void MainWindow::set_state(State s) {
    state_ = s;
    bool scanning = (s == State::Scanning);
    EnableWindow(hSearchBtn_, !scanning);
    EnableWindow(hCancelBtn_, scanning);
    EnableWindow(hBrowseBtn_, !scanning);
    EnableWindow(hCollectBtn_, !scanning);
    EnableWindow(hIndexBtn_, !scanning);
    // 关键词框：扫描中禁用搜索（EN_CHANGE 也会判断）
    std::wstring kw = trim(get_window_text(hKeywordEdit_));
    EnableWindow(hSearchBtn_, !scanning && !kw.empty());
    update_export_button();   // S9：结果集为空时导出按钮保持禁用（含首次启动）
}

// ============================================================
// 状态栏 / 进度
// ============================================================

void MainWindow::set_status(const std::wstring& text) {
    // S2 修复：状态栏文本超长时自动省略号截断（状态栏默认行为），
    // 并打上 SBT_TOOLTIP 标记——控件在文本被截断时自动弹出 ToolTip 显示全文，
    // 用户无需肉眼从半截字里猜「只对纳入的扩展名生效」那句重要提示。
#ifndef SBT_TOOLTIP
#define SBT_TOOLTIP 0x0800
#endif
    SendMessageW(hStatusBar_, SB_SETTEXTW,
                 MAKEWPARAM(3, SBT_TOOLTIP),
                 reinterpret_cast<LPARAM>(text.c_str()));
}

// ============================================================
// S1 结果排序
// ============================================================

void MainWindow::apply_sort() {
    // 空结果也要走列头箭头刷新：新搜索把 sort_col_/sort_asc_ 复位后，旧搜索
    // 的 ▲/▼ 若不清理，会在空列表/未排序列上残留误导。排序本身跳过。
    if (!results_.empty()) {
        int col = sort_col_;
        bool asc = sort_asc_;
        std::sort(results_.begin(), results_.end(),
            [col, asc](const MatchResult& a, const MatchResult& b) {
                int cmp;
                if (col == 1) {
                    cmp = (a.line_no < b.line_no) ? -1
                        : (a.line_no > b.line_no) ? 1 : 0;
                } else if (col == 2) {
                    cmp = a.line_text.compare(b.line_text);
                } else {  // 默认：文件路径
                    cmp = a.file_path.wstring().compare(b.file_path.wstring());
                }
                return asc ? (cmp < 0) : (cmp > 0);
            });
    }
    // 列头排序箭头指示（升序 ▲ / 降序 ▼）——排序箭头在 header 子控件上设置
    HWND hHeader = ListView_GetHeader(hResultList_);
    if (hHeader) {
        for (int i = 0; i < 3; ++i) {
            HDITEMW hi{};
            hi.mask = HDI_FORMAT;
            Header_GetItem(hHeader, i, &hi);
            hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
            if (i == sort_col_) {
                hi.fmt |= sort_asc_ ? HDF_SORTUP : HDF_SORTDOWN;
            }
            Header_SetItem(hHeader, i, &hi);
        }
    }
}

// 刷新状态栏「索引」pane。
// 无效路径（空/不存在/不是目录）→ 同步直接写「索引：路径框内是无效路径」。
// 有效路径 → 异步：index_exists 会 open+verify 索引文件（读 checksum），
// 在大索引下会卡 UI 线程几十~几百毫秒；改为后台线程 + PostMessage 回包，
// 并用代际号 index_check_gen_ 保证「用户切到新路径后旧检测结果不会覆盖新结果」。
// 同步调用方（如 on_search）需要立即拿到结果时，可直接调 Indexer::index_exists。
void MainWindow::update_index_status() {
    if (!hStatusBar_ || !hPathEdit_) return;
    std::wstring root = trim(get_window_text(hPathEdit_));
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        // 任何飞行中的旧检测结果作废
        index_check_gen_.fetch_add(1, std::memory_order_acq_rel);
        // 空路径与无效路径分别表述；pane 宽度有限，文案要短（旧文案
        // 「路径框内是无效路径」13 个字在 150px pane 里被裁成省略号）。
        SendMessageW(hStatusBar_, SB_SETTEXTW, MAKEWPARAM(2, 0),
                     (LPARAM)(root.empty() ? i18n::tr("sb.index_nopath")
                                           : i18n::tr("sb.index_badpath")));
        return;
    }
    // 启动异步检测
    uint64_t gen = index_check_gen_.fetch_add(1, std::memory_order_acq_rel) + 1;
    // S5.5：线程不捕获 this——只带值拷贝（gen/root）+ 生命周期 shared_ptr +
    // HWND 值；PostMessage 前校验 alive 与 IsWindow，MainWindow 析构后安全退出。
    HWND hwnd = hwnd_;
    std::thread([gen, root, hwnd, alive = ui_alive_]() {
        bool exists = Indexer::index_exists(root);
        if (!alive->load(std::memory_order_acquire)) return;
        if (!IsWindow(hwnd)) return;
        PostMessageW(hwnd, WM_INDEX_STATUS,
                     static_cast<WPARAM>(gen),
                     static_cast<LPARAM>(exists ? 1 : 0));
    }).detach();
}

void MainWindow::update_progress(size_t done, size_t total) {
    done_count_ = done;
    total_count_ = total;
    if (total > 0) {
        int pct = static_cast<int>((done * 100) / total);
        if (pct > 100) pct = 100;
        SendMessageW(hProgressBar_, PBM_SETPOS, pct, 0);
    } else {
        SendMessageW(hProgressBar_, PBM_SETPOS, 0, 0);
    }
    // S6：索引模式不逐文件扫描，core 只会发 (0,1)/(1,1)。显示成「1 / 1 文件」
    // 会让用户以为只搜了 1 个文件——改为显示「索引查询」。
    // total==0（尚未开始/已复位）显示「—」，不出现误导性的「0 / 0 文件」。
    std::wstring part1;
    if (index_mode_) {
        part1 = i18n::tr("sb.count_index");
    } else if (total == 0) {
        part1 = L"—";
    } else {
        part1 = i18n::trf("sb.count_files", std::to_wstring(done),
                          std::to_wstring(total));
    }
    SendMessageW(hStatusBar_, SB_SETTEXTW,
                 MAKEWPARAM(1, 0), reinterpret_cast<LPARAM>(part1.c_str()));
}

void MainWindow::update_result_label() {
    if (!hResultLabel_) return;
    // O9：结果触达上限被截断时，在结果列表标题处也给出可见提示——
    // 仅状态栏文本追加很容易被错过，列表标题就摆在结果正上方。
    std::wstring t = i18n::tr("label.result");
    if (results_capped_) t += i18n::tr("label.result_capped");
    SetWindowTextW(hResultLabel_, t.c_str());
}

void MainWindow::update_export_button() {
    // S9：无结果时禁用导出按钮（比点了才弹窗更早给反馈）。
    // 扫描中同样禁用：结果还在增长，此刻导出的「部分结果」易被当成全量
    // （与 S3 取消置灰同一顾虑）；扫描完成后自动恢复。
    if (!hExportBtn_) return;
    EnableWindow(hExportBtn_, !results_.empty() && state_ != State::Scanning);
}

// ============================================================
// 搜索流程
// ============================================================

std::vector<std::wstring> MainWindow::parse_list_field(HWND hEdit) const {
    std::vector<std::wstring> out;
    std::wstring text = get_window_text(hEdit);
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L'\r' || c == L'\n') {
            std::wstring t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    std::wstring t = trim(cur);
    if (!t.empty()) out.push_back(t);
    return out;
}

SearchOptions MainWindow::build_options() {
    SearchOptions opts;
    opts.root = get_window_text(hPathEdit_);
    opts.pattern = get_window_text(hKeywordEdit_);
    opts.recursive = (Button_GetCheck(hChkRecursive_) == BST_CHECKED);
    opts.use_regex = (Button_GetCheck(hChkRegex_) == BST_CHECKED);
    // "区分大小写"勾选 → ignore_case=false；未勾选 → ignore_case=true
    opts.ignore_case = (Button_GetCheck(hChkCase_) != BST_CHECKED);
    opts.whole_word = (Button_GetCheck(hChkWholeWord_) == BST_CHECKED);
    // 强制即时扫描：勾选则跳过索引走实时扫描（用于索引/实时两种模式的结果比对）
    opts.force_realtime = (Button_GetCheck(hChkRealtime_) == BST_CHECKED);

    // 阈值（S4：不再静默改默认——无效时明确提示用户用了什么值）
    std::wstring thr = trim(get_window_text(hThresholdEdit_));
    size_t v = 50;
    bool threshold_bad = false;
    if (thr.empty()) {
        threshold_bad = true; v = 50;          // 空 → 用默认 50
    } else {
        try {
            v = std::stoul(thr);
            if (v < 1) { threshold_bad = true; v = 1; }   // 0 / 过小 → 兜底 1
        } catch (...) {
            threshold_bad = true; v = 50;       // 非数字（理论上 ES_NUMBER 拦不住的极端情况）
        }
    }
    opts.bigfile_threshold_mb = v;
    if (threshold_bad) {
        set_status(L"提示：大文件阈值无效（需为整数且 ≥1），已按 " +
                   std::to_wstring(v) + L" MB 处理");
    }

    opts.blacklist = parse_list_field(hBlacklistEdit_);
    opts.wildcards = parse_list_field(hWildcardEdit_);
    opts.max_results = config_.max_results;
    opts.search_timeout_sec = config_.search_timeout_sec;
    // 索引模式搜索前自动增量更新（防目录变更后结果陈旧）。
    // core 侧有快速预检：无变化时只做轻量对比，不触发全量 load / save。
    opts.auto_update_index = config_.auto_update_index;
    return opts;
}

std::set<std::wstring> MainWindow::resolve_extensions() {
    std::set<std::wstring> whitelist = ext_mgr_.load_whitelist();
    // 左栏勾选的扩展名作为唯一来源。
    // 收集对话框关闭时已经把结果回写到左栏（checked_exts_ 已体现在复选框里），
    // 因此搜索时只读左栏实时勾选状态；再合并 checked_exts_ 会让旧勾选覆盖用户的取消动作。
    std::set<std::wstring> dir_checked;
    int n = ListView_GetItemCount(hExtList_);
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(hExtList_, i)) {
            wchar_t buf[64];
            ListView_GetItemText(hExtList_, i, 0, buf, 64);
            std::wstring ext = buf;
            // 左栏用「(无扩展名)」作无扩展名文件的显示占位；回读时必须还原
            // 为空串，否则占位文本会被当成真实扩展名参与过滤——无扩展名
            // 文件永远匹配不上（收集对话框勾了却搜不到的隐性断链）。
            if (ext == L"(无扩展名)") ext.clear();
            dir_checked.insert(ext);
        }
    }

    auto blacklist = parse_list_field(hBlacklistEdit_);
    auto wildcards = parse_list_field(hWildcardEdit_);
    auto ext_set = resolve_ext_set(whitelist, dir_checked, blacklist, wildcards);
    return ext_set;
}

void MainWindow::on_search() {
    if (state_ == State::Scanning) return;

    SearchOptions opts = build_options();
    std::wstring root = trim(opts.root.wstring());

    // F.1.4 / F.2.7：空路径 → 报错
    if (root.empty()) {
        set_status(i18n::tr("err.no_path"));
        SetFocus(hPathEdit_);
        return;
    }
    // F.1.4：路径不存在
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        set_status(i18n::tr("err.bad_path"));
        SetFocus(hPathEdit_);
        return;
    }
    opts.root = root;  // 使用去空白后的路径

    // F.2.7：空 pattern → 报错
    std::wstring pattern = opts.pattern;
    if (pattern.empty()) {
        set_status(i18n::tr("err.no_keyword"));
        SetFocus(hKeywordEdit_);
        return;
    }
    // F.1.4：最大长度限制 1000 字符
    if (pattern.size() > kMaxPatternLen) {
        pattern = pattern.substr(0, kMaxPatternLen);
        opts.pattern = pattern;
        set_status(i18n::tr("hint.long_kw"));
    }
    // F.1.4：正则 > 500 字符警告
    if (opts.use_regex && pattern.size() > 500) {
        set_status(i18n::tr("warn.long_regex"));
    }

    // F.2.7：正则预校验
    if (opts.use_regex) {
        RegexCheckResult rc = check_regex(pattern, opts.ignore_case);
        if (!rc.valid) {
            set_status(i18n::trf("err.regex", rc.message));
            SetFocus(hKeywordEdit_);
            return;
        }
    }

    // 构建扩展名集合
    auto ext_set = resolve_extensions();
    if (ext_set.empty()) {
        // 无勾选扩展名 → 回退到常用扩展名
        ext_set = ext_mgr_.load_whitelist();
        if (ext_set.empty()) {
            set_status(i18n::tr("err.no_ext"));
            return;
        }
        set_status(i18n::tr("hint.no_ext"));
    }

    // S5：所有前置校验（路径/关键词/正则/扩展名）都通过后才记入历史——
    // 中途因缺扩展名中止的搜索不该污染历史。
    push_keyword_history(pattern);

    // 搜索模式不在 UI 线程判定（index_exists 对大索引要读全文件算 checksum，
    // 会卡界面秒级）。改为：UI 先显示通用"正在扫描…"，worker 线程预判后
    // PostMessage(WM_SEARCH_MODE) 回传，自动补上「（索引模式）/（实时扫描）」。
    search_mode_label_.clear();

    // 清空结果
    results_.clear();
    results_partial_ = false;   // 新搜索开始 → 任何上一次「取消置灰」失效
    sort_col_ = 0;              // S1：每次新搜索回到默认排序（按路径）
    sort_asc_ = true;
    apply_sort();               // 列头箭头随排序复位（数据已清空，仅刷新箭头）
    index_mode_ = false;        // S6：模式由 worker 回传 WM_SEARCH_MODE 后确定
    results_capped_ = false;    // O9：清空结果的同时清除上限提示
    // 行号列「N/M」：新搜索重置总行缓存
    file_total_lines_.clear();
    update_result_label();
    update_export_button();     // S9：清空结果的同时禁用导出
    ListView_SetItemCountEx(hResultList_, 0, 0);
    skipped_count_ = 0;
    done_count_ = 0;
    total_count_ = 0;
    scan_notice_.clear();
    last_pattern_ = pattern;
    viewer_.clear();
    SetWindowTextW(hViewerLabel_, i18n::tr("label.preview"));

    // 切换到扫描状态
    user_cancelled_ = false;
    set_state(State::Scanning);
    set_status(i18n::tr("status.scanning"));
    SendMessageW(hProgressBar_, PBM_SETPOS, 0, 0);
    last_progress_tick_ = GetTickCount64();
    if (timer_id_) KillTimer(hwnd_, timer_id_);
    timer_id_ = SetTimer(hwnd_, kWatchdogTimerId, kWatchdogIntervalMs, nullptr);

    // 启动 worker 线程（T6 6.1）
    quitting_.store(false);
    scanner_ = std::make_shared<Scanner>();
    auto sc = scanner_;
    std::filesystem::path root_path = root;

    worker_thread_ = std::thread([this, sc, opts, ext_set, root_path]() {
        // 模式预判在 worker 线程做（与 core/scanner.cpp scan_realtime 内部路由一致：
        // !force_realtime && Indexer::index_exists）。index_exists 有 core 层
        // mtime/size 缓存，重复调用近零开销。
        bool use_index = !opts.force_realtime && Indexer::index_exists(root_path);
        if (!quitting_.load(std::memory_order_acquire)) {
            PostMessageW(hwnd_, WM_SEARCH_MODE, 0,
                         static_cast<LPARAM>(use_index ? 1 : 0));
        }

        ScanCallbacks cb;
        // 进度（WM_APP+1）
        cb.on_progress = [this](size_t done, size_t total) {
            if (quitting_.load(std::memory_order_acquire)) return;
            PostMessage(hwnd_, WM_PROGRESS, (WPARAM)done, (LPARAM)total);
        };
        // 结果批次（WM_APP+2）：F.2.9 批次聚合由 Scanner 完成
        cb.on_results = [this](std::vector<MatchResult> results) {
            if (quitting_.load(std::memory_order_acquire)) return;
            auto* p = new std::vector<MatchResult>(std::move(results));
            if (!PostMessage(hwnd_, WM_RESULTS, 0, reinterpret_cast<LPARAM>(p))) {
                delete p;  // F.2.9 窗口失效 → 释放
            }
        };
        // 错误/警告（WM_APP+3）：F.2.4 异常通过此回传
        cb.on_error = [this](const std::wstring& msg) {
            if (quitting_.load(std::memory_order_acquire)) return;
            auto* p = new std::wstring(msg);
            if (!PostMessage(hwnd_, WM_DONE, 1, reinterpret_cast<LPARAM>(p))) {
                delete p;
            }
        };
        // 跳过统计（WM_APP+4）：F.2.7 无权限/锁定/二进制
        cb.on_skip = [this](size_t skipped) {
            if (quitting_.load(std::memory_order_acquire)) return;
            PostMessage(hwnd_, WM_SKIP, (WPARAM)skipped, 0);
        };
        // 完成（WM_APP+3, status=0）
        cb.on_complete = [this]() {
            if (quitting_.load(std::memory_order_acquire)) return;
            PostMessage(hwnd_, WM_DONE, 0, 0);
        };

        // F.2.4：worker 入口 try/catch 兜底
        try {
            sc->scan_realtime(opts, ext_set, cb);
        } catch (const std::exception& e) {
            if (!quitting_.load(std::memory_order_acquire)) {
                auto* p = new std::wstring(
                    L"内部错误: " + utf8_to_wstring(e.what()));
                if (!PostMessage(hwnd_, WM_DONE, 1,
                                 reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            }
        } catch (...) {
            if (!quitting_.load(std::memory_order_acquire)) {
                auto* p = new std::wstring(L"未知内部错误");
                if (!PostMessage(hwnd_, WM_DONE, 1,
                                 reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            }
        }
    });

    // B2 修复：日志不再记录搜索关键词（可能是密码/API Key/个人信息），
    // 仅记录搜索范围路径（路径属工具固有作用域，敏感度低）。
    logger::info(L"开始搜索: 路径=" + root);
}

void MainWindow::on_cancel() {
    if (state_ != State::Scanning) return;
    if (scanner_) {
        user_cancelled_ = true;
        scanner_->cancel();
        set_status(i18n::tr("status.cancel_build"));
        logger::info(L"用户取消搜索");
    } else {
        // S3.1：建索引流程（无 Scanner）——置位取消。worker 尚未开跑则直接
        // 收口；建库已开始则无法中断（Indexer::build 无取消点），照常完成。
        user_cancelled_ = true;
        build_cancelled_.store(true, std::memory_order_release);
        set_status(i18n::tr("status.cancel_build2"));
        logger::info(L"用户取消建立索引");
    }
}

void MainWindow::on_browse() {
    // IFileDialog 选目录（Vista+）
    IFileOpenDialog* pfd = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr) && pfd) {
        DWORD flags;
        pfd->GetOptions(&flags);
        pfd->SetOptions(flags | FOS_PICKFOLDERS);
        pfd->SetTitle(L"选择搜索目录");
        if (SUCCEEDED(pfd->Show(hwnd_))) {
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
                PWSTR path = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    SetWindowTextW(hPathEdit_, path);
                    update_index_status();
                    CoTaskMemFree(path);
                }
                psi->Release();
            }
        }
        pfd->Release();
    } else {
        // 回退 SHBrowseForFolder（老式）
        BROWSEINFOW bi{};
        bi.hwndOwner = hwnd_;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
        bi.lpszTitle = L"选择搜索目录";
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t buf[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, buf)) {
                SetWindowTextW(hPathEdit_, buf);
                update_index_status();
            }
            CoTaskMemFree(pidl);
        }
    }
}

void MainWindow::on_collect_extensions() {
    std::wstring root = trim(get_window_text(hPathEdit_));
    if (root.empty() || !std::filesystem::is_directory(root)) {
        MessageBoxW(hwnd_, i18n::tr("mb.no_path"), i18n::tr("note"),
                    MB_OK | MB_ICONINFORMATION);
        SetFocus(hPathEdit_);
        return;
    }

    set_status(i18n::tr("status.collecting"));
    // 收集可能在 UI 线程阻塞，但通常较快；保持简单同步执行
    std::set<std::wstring> whitelist = ext_mgr_.load_whitelist();
    std::set<std::wstring> checked;
    bool ok = dlg_extensions::show(hwnd_, hInst_, root, whitelist, checked);
    if (ok) {
        checked_exts_ = checked;
        // 同步到左栏列表：显示勾选的扩展名
        ListView_DeleteAllItems(hExtList_);
        int idx = 0;
        // 合并常用扩展名与勾选，全部展示，勾选状态反映 checked
        std::set<std::wstring> all = whitelist;
        for (const auto& e : checked) all.insert(e);
        for (const auto& e : all) {
            std::wstring label = e;
            if (label.empty()) label = L"(无扩展名)";
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = idx;
            lvi.iSubItem = 0;
            lvi.pszText = const_cast<LPWSTR>(label.c_str());
            ListView_InsertItem(hExtList_, &lvi);
            bool isChecked = (checked.find(e) != checked.end()) ||
                             (e.empty() && checked.count(L""));
            ListView_SetCheckState(hExtList_, idx, isChecked ? TRUE : FALSE);
            ++idx;
        }
        set_status(i18n::tr("status.collected"));
    } else {
        set_status(i18n::tr("status.collect_cancel"));
    }
}

void MainWindow::on_build_index() {
    std::wstring root = trim(get_window_text(hPathEdit_));
    if (root.empty() || !std::filesystem::is_directory(root)) {
        MessageBoxW(hwnd_, i18n::tr("mb.no_path"), i18n::tr("mb.index_title"),
                    MB_OK | MB_ICONINFORMATION);
        SetFocus(hPathEdit_);
        return;
    }

    // 建索引不设搜索模式标签（on_done 最终文本不追加「（索引模式）」）
    search_mode_label_.clear();

    // 收集扩展名：左栏勾选优先；未勾选任何项时回退常用扩展名全集
    bool fallback_whitelist = false;
    auto ext_set = resolve_extensions();
    if (ext_set.empty()) {
        ext_set = ext_mgr_.load_whitelist();
        fallback_whitelist = true;   // 静默回退改为显式警告（见下方确认框）
    }
    if (ext_set.empty()) {
        MessageBoxW(hwnd_, i18n::tr("mb.no_ext"),
                    i18n::tr("mb.index_title"), MB_OK | MB_ICONWARNING);
        return;
    }

    // 建前摘要确认：让用户看到将索引哪些类型；常用扩展名回退时明确警告全量建库
    std::wstring summary;
    for (const auto& e : ext_set) {
        if (!summary.empty()) summary += L" ";
        summary += e.empty() ? L"(无扩展名)" : e;
    }
    std::wstring msg = i18n::trf("mb.index_summary", root,
                       std::to_wstring(ext_set.size()), summary);
    if (fallback_whitelist) {
        msg += i18n::trf("mb.index_fallback",
                         std::to_wstring(ext_set.size()));
    }
    if (MessageBoxW(hwnd_, msg.c_str(), i18n::tr("mb.index_title"),
                    MB_OKCANCEL |
                        (fallback_whitelist ? MB_ICONWARNING : MB_ICONINFORMATION)
                    ) != IDOK) {
        set_status(i18n::tr("mb.cancelled_ok"));
        logger::info(L"用户取消建立索引: " + root);
        return;
    }

    // 切换到扫描状态（复用 Scanning 状态机禁用按钮）
    set_state(State::Scanning);
    set_status(i18n::tr("status.indexing"));
    SendMessageW(hProgressBar_, PBM_SETPOS, 0, 0);
    // 复位上一次搜索遗留的模式与计数显示，避免建库期间/结束后状态栏
    // 仍显示「索引查询」或「N / M 文件」这类与建库无关的旧值
    index_mode_ = false;
    done_count_ = 0;
    total_count_ = 0;
    update_progress(0, 0);
    user_cancelled_ = false;
    quitting_.store(false);
    build_cancelled_.store(false, std::memory_order_release);

    // 在 worker 线程中跑 idx.build()，通过 WM_DONE 回传结果
    // S3.1：本流程所有收口消息一律 status=0（含失败/异常）。此前失败发
    // status=1「孤包」——on_done 对 status=1 不切状态、不 join worker，
    // UI 永久卡在 Scanning。status=0 + 非空消息 = 正常收尾并把消息作为
    // 最终状态文本（on_done 的建库分支原生支持）。
    std::filesystem::path root_path = root;
    auto t0 = std::chrono::steady_clock::now();
    size_t ext_count = ext_set.size();
    worker_thread_ = std::thread([this, root_path, ext_set, ext_count, t0]() {
        // 开始前检查取消位（建库已开始则无法中断）
        if (build_cancelled_.load(std::memory_order_acquire)) {
            if (!quitting_.load(std::memory_order_acquire)) {
                PostMessage(hwnd_, WM_DONE, 0, 0);   // 空消息 → 「已取消建立索引」
            }
            return;
        }
        try {
            Indexer idx;
            // B3：建库进度回传（done/total = 已索引/待索引文件数）。
            // build 的并行 worker 会并发调用本回调——PostMessage 线程安全，
            // UI 侧复用 WM_PROGRESS 通道（进度条 + 计数 pane）。
            Indexer::BuildProgress prog = [this](size_t done, size_t total) {
                if (quitting_.load(std::memory_order_acquire)) return;
                if (!IsWindow(hwnd_)) return;
                PostMessageW(hwnd_, WM_PROGRESS,
                             static_cast<WPARAM>(done),
                             static_cast<LPARAM>(total));
            };
            bool ok = idx.build(root_path, ext_set, prog);
            if (quitting_.load(std::memory_order_acquire)) return;
            auto t1 = std::chrono::steady_clock::now();
            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                t1 - t0).count();
            std::wstring dur = format_duration_ms(ms);
            std::wstring* p = nullptr;
            if (ok) {
                p = new std::wstring(
                    i18n::trf("status.index_done", dur,
                              std::to_wstring(ext_count)));
            } else {
                p = new std::wstring(i18n::tr("err.index_fail"));
            }
            if (!PostMessage(hwnd_, WM_DONE, 0, reinterpret_cast<LPARAM>(p))) {
                delete p;
            }
        } catch (const std::exception& e) {
            if (!quitting_.load(std::memory_order_acquire)) {
                auto* p = new std::wstring(
                    i18n::trf("err.index_exc",
                              utf8_to_wstring(e.what())));
                if (!PostMessage(hwnd_, WM_DONE, 0,
                                 reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            }
        } catch (...) {
            if (!quitting_.load(std::memory_order_acquire)) {
                auto* p = new std::wstring(i18n::tr("err.index_unk"));
                if (!PostMessage(hwnd_, WM_DONE, 0,
                                 reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            }
        }
    });

    logger::info(L"开始建立索引: " + root);
}

// ============================================================
// 结果交互
// ============================================================

int MainWindow::selected_result_index() const {
    int sel = ListView_GetNextItem(hResultList_, -1, LVNI_SELECTED);
    return sel;
}

void MainWindow::on_result_activate() {
    int sel = selected_result_index();
    if (sel < 0 || sel >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<size_t>(sel)];

    bool use_regex = (Button_GetCheck(hChkRegex_) == BST_CHECKED);
    bool ignore_case = (Button_GetCheck(hChkCase_) != BST_CHECKED);
    bool whole_word = (Button_GetCheck(hChkWholeWord_) == BST_CHECKED);

    // 双击 → 预览定位到该行的第一个标黄处（正则模式不标黄，退化为行首定位 R20）。
    // pattern 全量传入：Viewer 用同一匹配引擎统计全文命中数（标题展示）。
    viewer_.show_file(r.file_path, r.line_no, last_pattern_, ignore_case,
                      use_regex, whole_word);
    // 更新右栏标题：文件名 [编码] · N 处命中（标黄数少于命中数时注明）
    std::wstring title = i18n::trf("preview.title", viewer_.file_label(),
                                   viewer_.encoding_label());
    if (!last_pattern_.empty()) {
        title += i18n::trf("preview.hits",
                           std::to_wstring(viewer_.hit_count()));
        if (viewer_.highlights_capped()) {
            title += i18n::trf("preview.hl",
                               std::to_wstring(viewer_.highlight_count()));
        }
    }
    SetWindowTextW(hViewerLabel_, title.c_str());
    // S8：预览失败必须显式告知。旧版只在右栏静默写一句「文件为空或无法读取」，
    // 用户往往以为是自己搜错了，实际是目标文件已被删/无权限/过大。
    if (!viewer_.last_error().empty()) {
        SetWindowTextW(hViewerLabel_,
            i18n::trf("preview.fail", viewer_.last_error()).c_str());
        set_status(i18n::trf("preview.fail", viewer_.last_error()));
    }
    logger::info(L"查看: " + r.file_path.wstring() +
                 L" 行 " + std::to_wstring(r.line_no));
}

// 写入剪贴板（CF_UNICODETEXT）。成功返回 true；失败时调用方给状态栏反馈，
// 避免像旧代码那样静默失败（剪贴板被别的进程占用时很常见）。
static bool put_clipboard_text(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    do {
        if (!EmptyClipboard()) break;
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!hMem) break;
        wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
        if (!dst) { GlobalFree(hMem); break; }
        memcpy(dst, text.data(), text.size() * sizeof(wchar_t));
        dst[text.size()] = L'\0';
        GlobalUnlock(hMem);
        ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
    } while (false);
    CloseClipboard();
    return ok;
}

void MainWindow::copy_selected_result() {
    int sel = selected_result_index();
    if (sel < 0 || sel >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<size_t>(sel)];
    // F.1.2 格式：路径:行号\t命中行文本
    std::wstring line = r.file_path.wstring() + L":" +
                        std::to_wstring(r.line_no) + L"\t" + r.line_text;
    if (put_clipboard_text(hwnd_, line))
        set_status(i18n::tr("status.copied_line"));
    else
        set_status(i18n::tr("err.clipboard"));
}

// S7：只复制某一列（0=完整路径 1=行号），省掉手工从「路径:行号\t内容」里摘
void MainWindow::copy_result_field(int which) {
    int sel = selected_result_index();
    if (sel < 0 || sel >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<size_t>(sel)];
    std::wstring text;
    const wchar_t* what = L"";
    switch (which) {
        case 0:  text = r.file_path.wstring();     what = i18n::tr("ctx.copy_path"); break;
        case 1:  text = std::to_wstring(r.line_no); what = i18n::tr("ctx.copy_line_no"); break;
        default: return;
    }
    if (put_clipboard_text(hwnd_, text))
        set_status(what);
    else
        set_status(i18n::trf("err.clipboard2", what));
}

// S7：在资源管理器里定位并选中该结果文件
void MainWindow::open_containing_folder() {
    int sel = selected_result_index();
    if (sel < 0 || sel >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<size_t>(sel)];
    std::error_code ec;
    if (!std::filesystem::exists(r.file_path, ec)) {
        set_status(i18n::tr("err.gone"));
        return;
    }
    // explorer /select,"<path>" 是唯一能「打开文件夹并选中指定文件」的方式
    std::wstring args = L"/select,\"" + r.file_path.wstring() + L"\"";
    HINSTANCE rc = ShellExecuteW(hwnd_, L"open", L"explorer", args.c_str(),
                                 nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(rc) <= 32) {
        set_status(L"打开文件夹失败（ShellExecute 错误 " +
                   std::to_wstring(reinterpret_cast<INT_PTR>(rc)) + L"）");
    }
}

// 「打开编辑」：调用系统默认关联工具打开选中的文件；无关联（SE_ERR_NOASSOC）
// 时自动回退记事本（本工具只针对文本型文件，记事本必然可开）。
void MainWindow::open_in_editor() {
    int sel = selected_result_index();
    if (sel < 0 || sel >= static_cast<int>(results_.size())) return;
    const auto& r = results_[static_cast<size_t>(sel)];
    std::error_code ec;
    if (!std::filesystem::exists(r.file_path, ec)) {
        set_status(i18n::tr("err.gone"));
        return;
    }
    std::wstring file = r.file_path.wstring();
    HINSTANCE rc = ShellExecuteW(hwnd_, L"open", file.c_str(),
                                 nullptr, nullptr, SW_SHOWNORMAL);
    INT_PTR code = reinterpret_cast<INT_PTR>(rc);
    if (code > 32) return;   // 成功
    if (code == SE_ERR_NOASSOC) {
        std::wstring msg = i18n::trf("mb.no_default_editor",
                               r.file_path.filename().wstring());
        MessageBoxW(hwnd_, msg.c_str(), i18n::tr("ctx.open_edit"),
                    MB_OK | MB_ICONINFORMATION);
        HINSTANCE rc2 = ShellExecuteW(hwnd_, L"open", L"notepad.exe",
                                      file.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(rc2) <= 32) {
            set_status(i18n::trf("mb.open_fail",
                       std::to_wstring(reinterpret_cast<INT_PTR>(rc2))));
        }
        return;
    }
    set_status(i18n::trf("mb.open_fail", std::to_wstring(code)));
}

// S7：结果列表右键菜单。has_sel=false（点空白处/空列表）时全部置灰而非静默
// ---- S5：关键词历史（仅内存，B2：关键词绝不落盘）----

void MainWindow::push_keyword_history(const std::wstring& kw) {
    if (kw.empty() || kw.size() > kMaxPatternLen) return;
    // 已有则先移除旧位置 → 提升到最前（最近使用在前）
    keyword_history_.erase(
        std::remove(keyword_history_.begin(), keyword_history_.end(), kw),
        keyword_history_.end());
    keyword_history_.insert(keyword_history_.begin(), kw);
    int hist_max = config_.history_max > 0 ? config_.history_max : 10;
    if ((int)keyword_history_.size() > hist_max)
        keyword_history_.resize(static_cast<size_t>(hist_max));
}

void MainWindow::show_keyword_history() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    if (keyword_history_.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
                    i18n::tr("hist.empty"));
    } else {
        for (size_t i = 0; i < keyword_history_.size(); ++i) {
            // 菜单里截断超长关键词，避免弹出一个横跨屏幕的巨宽菜单
            std::wstring text = keyword_history_[i];
            if (text.size() > 48) text = text.substr(0, 46) + L"…";
            AppendMenuW(hMenu, MF_STRING,
                        kHistoryCmdBase + static_cast<WORD>(i), text.c_str());
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0,
                i18n::tr("hist.note"));

    // 在按钮正下方左对齐弹出
    RECT rc{};
    GetWindowRect(hHistoryBtn_, &rc);
    TrackPopupMenuEx(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                     rc.left, rc.bottom, hwnd_, nullptr);
    DestroyMenu(hMenu);
}

void MainWindow::show_result_context_menu(POINT pt, bool has_sel) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    auto add = [&](UINT id, const wchar_t* text) {
        AppendMenuW(hMenu, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED),
                    id, text);
    };
    add(CTX_PREVIEW,     i18n::tr("ctx.preview"));
    add(CTX_OPEN_FOLDER, i18n::tr("ctx.open_folder"));
    add(CTX_OPEN_EDIT,   i18n::tr("ctx.open_edit"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    add(CTX_COPY_PATH,   i18n::tr("ctx.copy_path"));
    add(CTX_COPY_LINENO, i18n::tr("ctx.copy_line_no"));
    add(CTX_COPY_LINE,   i18n::tr("ctx.copy_all"));
    if (has_sel) SetMenuDefaultItem(hMenu, CTX_PREVIEW, FALSE);
    TrackPopupMenuEx(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                     pt.x, pt.y, hwnd_, nullptr);
    DestroyMenu(hMenu);
}

// ============================================================
// 自定义消息处理
// ============================================================

// 行号列固定宽（10 字符文本度量 + 边距）。create_controls 设定、
// load_settings 恢复旧记忆时钳制下限（防窄版旧配置把「N/M」截断）。
int MainWindow::lineno_col_fixed_width() {
    if (!hResultList_) return scale_dpi(90);
    HDC dc = GetDC(hResultList_);
    int w = scale_dpi(90);
    if (dc) {
        HFONT f = reinterpret_cast<HFONT>(
            SendMessageW(hResultList_, WM_GETFONT, 0, 0));
        HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, L"8888888888", 10, &sz) && sz.cx > 0)
            w = sz.cx + scale_dpi(10);
        if (old) SelectObject(dc, old);
        ReleaseDC(hResultList_, dc);
    }
    return w;
}

// 文件总行数（懒计算 + 缓存）。流式 1MB 分块计数，内存有界。
// 口径与查看器 split_lines 一致：行数 = 换行符数 +（末行无换行符 ? 1 : 0）。
// UTF-8/GBK 按 0x0A 字节计（GBK 尾字节不含 0x0A，与解码后一致）；
// UTF-16 按「码元对齐的 0x0A 00 / 00 0A」计；BOM 只看前 2 字节。
int MainWindow::file_line_total(const std::filesystem::path& p) {
    auto key = p.wstring();
    auto it = file_total_lines_.find(key);
    if (it != file_total_lines_.end()) return it->second;

    int total = 0;
    std::error_code ec;
    auto fsz = std::filesystem::file_size(p, ec);
    if (!ec && fsz > 0) {
        std::ifstream ifs(p, std::ios::binary);
        if (ifs) {
            unsigned char b0 = 0, b1 = 0;
            ifs.read(reinterpret_cast<char*>(&b0), 1);
            ifs.read(reinterpret_cast<char*>(&b1), 1);
            const bool be = (b0 == 0xFE && b1 == 0xFF);
            const bool utf16 = be || (b0 == 0xFF && b1 == 0xFE);
            ifs.seekg(0, std::ios::beg);

            std::vector<char> buf(1u << 20);
            unsigned long long off = 0;
            bool last_is_nl = false;
            while (ifs) {
                ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                std::streamsize got = ifs.gcount();
                if (got <= 0) break;
                if (utf16) {
                    size_t start = (off == 0) ? 2 : 0;      // 跳过 BOM
                    size_t end = static_cast<size_t>(got) & ~size_t(1);
                    for (size_t i = start; i + 1 < end + 1; i += 2) {
                        unsigned lo = be ? static_cast<unsigned char>(buf[i + 1])
                                         : static_cast<unsigned char>(buf[i]);
                        unsigned hi = be ? static_cast<unsigned char>(buf[i])
                                         : static_cast<unsigned char>(buf[i + 1]);
                        if (lo == 0x0A && hi == 0x00) ++total;
                    }
                    if (end >= 2) {
                        unsigned lo = be ? static_cast<unsigned char>(buf[end - 2])
                                         : static_cast<unsigned char>(buf[end - 1]);
                        unsigned hi = be ? static_cast<unsigned char>(buf[end - 1])
                                         : static_cast<unsigned char>(buf[end - 2]);
                        last_is_nl = (lo == 0x0A && hi == 0x00);
                    }
                } else {
                    total += static_cast<int>(std::count(
                        buf.begin(), buf.begin() + got, '\n'));
                    last_is_nl = (buf[got - 1] == '\n');
                }
                off += static_cast<unsigned long long>(got);
            }
            bool empty_doc = utf16 ? (fsz <= 2) : false;   // 仅 BOM → 0 行
            if (!empty_doc && !last_is_nl) ++total;        // 末行无换行符
        }
    }
    file_total_lines_.emplace(key, total);
    return total;
}


void MainWindow::on_progress(size_t done, size_t total) {
    last_progress_tick_ = GetTickCount64();
    update_progress(done, total);
}

void MainWindow::on_results(std::vector<MatchResult>* results) {
    std::unique_ptr<std::vector<MatchResult>> guard(results);
    if (!results) return;
    size_t old = results_.size();
    for (auto& r : *results) {
        results_.push_back(std::move(r));
    }
    ListView_SetItemCountEx(hResultList_, static_cast<int>(results_.size()), 0);
    update_export_button();   // S9：首批结果到达即可导出
    // 提示命中数
    if (!results->empty() || old != results_.size()) {
        std::wstring status = i18n::trf("status.scan_hits",
                              std::to_wstring(results_.size()));
        if (skipped_count_ > 0) {
            status += i18n::trf("status.skipped",
                                std::to_wstring(skipped_count_));
        }
        status += search_mode_label_;
        set_status(status);
    }
}

void MainWindow::on_skip(size_t skipped) {
    skipped_count_ = skipped;
    if (state_ == State::Scanning) {
        std::wstring status = i18n::trf("status.scan_hits",
                              std::to_wstring(results_.size())) +
                              i18n::trf("status.skipped",
                                        std::to_wstring(skipped)) +
                              search_mode_label_;
        set_status(status);
    }
}

void MainWindow::on_done(int status, std::wstring* msg) {
    std::unique_ptr<std::wstring> msg_guard(msg);
    if (msg && !msg->empty()) {
        // F.2.7 / F.2.4：错误/警告写入状态栏与日志
        set_status(*msg);
        scan_notice_ = *msg;
        // O9：结果达上限 → 在结果列表标题上也给出可见提示
        if (msg->find(L"结果已达上限") != std::wstring::npos) {
            results_capped_ = true;
            update_result_label();
        }
        logger::warn(*msg);
    }

    if (status == 0) {
        // 完成（正常或取消）→ 切回 Idle
        bool was_cancelled = user_cancelled_;
        user_cancelled_ = false;
        if (timer_id_) { KillTimer(hwnd_, timer_id_); timer_id_ = 0; }

        // 等 worker 线程退出（此时 scan_realtime 已返回）
        if (worker_thread_.joinable()) worker_thread_.join();
        scanner_.reset();

        set_state(State::Idle);
        update_progress(done_count_, total_count_);
        update_index_status();   // 索引建完/搜索结束 → 刷新「索引：已建立/未建立」

        if (search_mode_label_.empty()) {
            // 建索引流程（on_build_index 清空了模式标签）：直接用建索引消息，
            // 不套「已完成（命中 N 条）」前缀（N 是上次搜索的陈旧值，会误导）
            std::wstring text = scan_notice_;
            if (text.empty()) {
                text = was_cancelled ? i18n::tr("mb.cancelled_ok")
                                     : i18n::tr("status.index_done");
            } else if (was_cancelled && text.rfind(L"索引已建立", 0) == 0) {
                // 建库一旦开始无法中断：取消请求落空但索引实际建成了。
                // 如实报告「建成」并解释取消为何没生效——旧文案
                // 「已取消建立索引 — 索引已建立」自相矛盾，用户无从判断。
                text += L"（此前的取消未生效：建库开始后无法中断）";
            }
            set_status(text);
            logger::info(text);
            scan_notice_.clear();
            return;
        }

        // 最终状态文本（搜索流程：带上本次搜索的实际模式）
        // S3：取消时明确标注「结果为部分」，避免用户误把部分结果当全部。
        std::wstring text =
            (was_cancelled ? i18n::tr("status.cancelled") : i18n::tr("status.done")) +
            i18n::trf("status.hits", std::to_wstring(results_.size()));
        if (skipped_count_ > 0) {
            text += i18n::trf("status.skipped",
                              std::to_wstring(skipped_count_));
        }
        text += L")";
        text += search_mode_label_;
        // S3：结果列表置灰标记（自定义绘制据此整列变灰）
        results_partial_ = was_cancelled;
        // S1：最终列表按当前排序（默认按路径）重排并刷新
        apply_sort();
        update_export_button();   // S9：结果集定稿后同步导出按钮可用性
        if (hResultList_ && !results_.empty()) {
            ListView_RedrawItems(hResultList_, 0,
                                 static_cast<int>(results_.size()) - 1);
        }
        // 若扫描期间有重要通知（如结果达上限），追加展示
        if (!scan_notice_.empty()) {
            text += L"  — " + scan_notice_;
        }
        set_status(text);
        scan_notice_.clear();
        logger::info(text);
        // S3：取消 → 结果列表整列置灰（自定义绘制已读 results_partial_，
        // 这里强制重画一次，否则已显示的旧结果不会即时变灰）
        if (hResultList_ && results_partial_) {
            ListView_RedrawItems(hResultList_, 0,
                                 static_cast<int>(results_.size()) - 1);
        }
    }
    // status == 1（错误/警告）：不切状态，扫描继续；on_complete(status=0) 会负责收尾
}

// ============================================================
// 选项条折叠
// ============================================================

void MainWindow::toggle_options() {
    options_expanded_ = !options_expanded_;
    SetWindowTextW(hOptToggle_, options_expanded_ ? i18n::tr("opt.open")
                                                  : i18n::tr("opt.closed"));
    // O3：记住折叠状态（save_settings 在退出时落盘）
    config_.options_collapsed = !options_expanded_;
    layout_controls();
}

// ============================================================
// 结果导出（F.1.2）
// ============================================================

void MainWindow::on_export_results() {
    if (results_.empty()) {
        MessageBoxW(hwnd_, i18n::tr("mb.no_export"),
                    i18n::tr("mb.export_title"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t szFile[MAX_PATH] = L"textsearch_results.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (!GetSaveFileNameW(&ofn)) return;

    // 根据扩展名判断格式
    std::filesystem::path out_path = szFile;
    std::wstring ext = out_path.extension().wstring();
    bool csv = (ext == L".csv");

    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        MessageBoxW(hwnd_, i18n::tr("mb.export_fail"),
                    i18n::tr("mb.export_fail_title"),
                    MB_OK | MB_ICONERROR);
        return;
    }

    // 字段：文件路径、行号、列号、命中行全文、匹配关键词
    if (csv) {
        // 写入 UTF-8 BOM 以便 Excel 正确识别
        const char bom[] = {static_cast<char>(0xEF), static_cast<char>(0xBB),
                            static_cast<char>(0xBF)};
        ofs.write(bom, 3);
        ofs << "文件路径,行号,列号,命中行全文,匹配关键词\r\n";
        for (const auto& r : results_) {
            std::string fpath = wstring_to_utf8(csv_escape(r.file_path.wstring()));
            std::string lineno = std::to_string(r.line_no);
            std::string col = std::to_string(r.col);
            std::string linetext = wstring_to_utf8(csv_escape(r.line_text));
            std::string kw = wstring_to_utf8(csv_escape(last_pattern_));
            ofs << fpath << "," << lineno << "," << col << ","
                << linetext << "," << kw << "\r\n";
        }
    } else {
        // TXT：制表符分隔
        ofs << "文件路径\t行号\t列号\t命中行全文\t匹配关键词\r\n";
        for (const auto& r : results_) {
            std::string fpath = wstring_to_utf8(r.file_path.wstring());
            ofs << fpath << "\t" << r.line_no << "\t" << r.col << "\t"
                << wstring_to_utf8(r.line_text) << "\t"
                << wstring_to_utf8(last_pattern_) << "\r\n";
        }
    }
    ofs.close();

    set_status(i18n::trf("status.exported",
                std::to_wstring(results_.size()),
                out_path.filename().wstring()));
    logger::info(L"导出结果: " + out_path.wstring() +
                 L" (" + std::to_wstring(results_.size()) + L" 条)");
}

// ============================================================
// 快捷键
// ============================================================

void MainWindow::create_accelerators() {
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'L',            CMD_FOCUS_PATH },
        { FCONTROL | FVIRTKEY, 'F',            CMD_FOCUS_KEYWORD },
        { FCONTROL | FVIRTKEY, 'E',            CMD_EXPORT },
        { FVIRTKEY,            VK_ESCAPE,      CMD_CANCEL },
        { FALT | FVIRTKEY,     '1',            CMD_FOCUS_LEFT },
        { FALT | FVIRTKEY,     '2',            CMD_FOCUS_MIDDLE },
        { FALT | FVIRTKEY,     '3',            CMD_FOCUS_RIGHT },
    };
    hAccel_ = CreateAcceleratorTableW(accels, static_cast<int>(sizeof(accels) / sizeof(ACCEL)));
}

void MainWindow::focus_column(int which) {
    switch (which) {
        case 1: SetFocus(hExtList_); break;
        case 2: SetFocus(hResultList_); break;
        case 3: SetFocus(viewer_.hwnd()); break;
    }
}

// ============================================================
// 看门狗（F.2.4）
// ============================================================

void MainWindow::on_watchdog() {
    if (state_ != State::Scanning) return;
    // 索引模式全程只有 (0,1)/(1,1) 两个进度点：其间的增量更新 + 索引查询
    // 可能长时间无进度回传，且无法中途中断——不弹「停滞」误报（60s 阈值
    // 是按实时模式逐文件进度的口径定的）。
    if (index_mode_) return;
    ULONGLONG now = GetTickCount64();
    ULONGLONG watchdog_ms =
        static_cast<ULONGLONG>(config_.watchdog_sec > 0 ? config_.watchdog_sec : 60) * 1000ULL;
    if (watchdog_ms <= 0) watchdog_ms = 60000;
    if (now - last_progress_tick_ >= watchdog_ms) {
        // 60s 无进度消息 → 提示
        int ret = MessageBoxW(hwnd_,
            i18n::tr("watchdog.msg"), i18n::tr("watchdog.title"),
            MB_YESNO | MB_ICONWARNING);
        if (ret == IDYES) {
            on_cancel();
        }
        // 重置计时，避免反复弹窗
        last_progress_tick_ = now;
    }
}

// ============================================================
// 配置持久化（F.1.5）
// ============================================================

void MainWindow::load_settings() {
    // 恢复上次路径（关键词不再持久化，避免明文落盘 —— B2 修复）
    if (!config_.last_root.empty()) {
        SetWindowTextW(hPathEdit_, config_.last_root.c_str());
    }
    // 选项
    Button_SetCheck(hChkRecursive_, config_.recursive ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hChkRegex_, config_.use_regex ? BST_CHECKED : BST_UNCHECKED);
    // ignore_case=true → "区分大小写"未勾选
    Button_SetCheck(hChkCase_, config_.ignore_case ? BST_UNCHECKED : BST_CHECKED);
    Button_SetCheck(hChkWholeWord_, config_.whole_word ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(hChkRealtime_, config_.force_realtime ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(hThresholdEdit_, std::to_wstring(config_.threshold_mb).c_str());
    // 三栏列宽（0 → layout_controls 中赋默认值）
    if (config_.left_col_w > 0) left_col_w_ = config_.left_col_w;
    if (config_.right_col_w > 0) right_col_w_ = config_.right_col_w;

    // O3：恢复选项条折叠状态（此前每次启动都回到展开）
    options_expanded_ = !config_.options_collapsed;
    SetWindowTextW(hOptToggle_, options_expanded_ ? i18n::tr("opt.open")
                                                  : i18n::tr("opt.closed"));

    // 预览自动换行：恢复上次选择（默认开；纯显示折行，行号仍按文件真实行）
    if (hWrapChk_) {
        Button_SetCheck(hWrapChk_,
                        config_.preview_word_wrap ? BST_CHECKED
                                                  : BST_UNCHECKED);
    }
    viewer_.set_word_wrap(config_.preview_word_wrap);
    // 语言组合框选中项（配置 "system"/"zh"/"en" → 0/1/2）
    if (hLangCombo_) {
        int sel = 0;
        if (config_.ui_lang == L"zh") sel = 1;
        else if (config_.ui_lang == L"en") sel = 2;
        SendMessageW(hLangCombo_, CB_SETCURSEL, sel, 0);
    }

    // O5：恢复结果列表三列宽度（>0 才覆盖默认，否则保留 create_controls 的缺省）。
    // 行号列（「命中行/文件总行」）有固定最小宽——旧配置里窄版行号列的记忆
    // 会把 create_controls 设好的 10 字符宽覆盖回「82/2...」截断（2026-10-27）。
    if (hResultList_) {
        if (config_.result_col0_w > 0)
            ListView_SetColumnWidth(hResultList_, 0, config_.result_col0_w);
        if (config_.result_col1_w > 0)
            ListView_SetColumnWidth(hResultList_, 1,
                                    std::max(config_.result_col1_w,
                                             lineno_col_fixed_width()));
        if (config_.result_col2_w > 0)
            ListView_SetColumnWidth(hResultList_, 2, config_.result_col2_w);
    }
}

void MainWindow::save_settings() {
    // 窗口位置/大小
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd_, &wp);
    config_.window_maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    RECT rc = wp.rcNormalPosition;
    config_.window_x = rc.left;
    config_.window_y = rc.top;
    config_.window_w = rc.right - rc.left;
    config_.window_h = rc.bottom - rc.top;

    // 搜索参数
    config_.last_root = get_window_text(hPathEdit_);
    // B2：不保存关键词（config_ 已无 last_pattern 字段）
    config_.recursive = (Button_GetCheck(hChkRecursive_) == BST_CHECKED);
    config_.use_regex = (Button_GetCheck(hChkRegex_) == BST_CHECKED);
    config_.ignore_case = (Button_GetCheck(hChkCase_) != BST_CHECKED);
    config_.whole_word = (Button_GetCheck(hChkWholeWord_) == BST_CHECKED);
    config_.force_realtime = (Button_GetCheck(hChkRealtime_) == BST_CHECKED);
    std::wstring thr = trim(get_window_text(hThresholdEdit_));
    try {
        config_.threshold_mb = std::stoul(thr);
    } catch (...) {
        config_.threshold_mb = 50;
    }

    // 三栏列宽
    config_.left_col_w = left_col_w_;
    config_.right_col_w = right_col_w_;

    // O5：结果列表列宽（用户拖拽列头改变宽度后随窗口退出落盘）
    if (hResultList_) {
        config_.result_col0_w = static_cast<int>(ListView_GetColumnWidth(hResultList_, 0));
        config_.result_col1_w = static_cast<int>(ListView_GetColumnWidth(hResultList_, 1));
        config_.result_col2_w = static_cast<int>(ListView_GetColumnWidth(hResultList_, 2));
    }

    save_config(config_);
}
