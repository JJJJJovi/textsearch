// gui/viewer.cpp — 右栏只读查看器实现（T5 5.4d）
// RichEdit（RichEdit20W）：命中关键词黄底黑字标黄（F.1.6）+ 滚动定位。
// 2026-10-26 增：
//   1) 自动换行（EM_SETTARGETDEVICE 切换，界面复选框可切、退出记忆）
//   2) 左侧行号槽（自绘子窗口；EM_POSFROMCHAR 取各段落起始 y，自动换行时
//      折行续行不显示行号——与主流编辑器一致）
//   3) 全文命中计数（core/matcher，与结果列表同口径；正则计数限时 5s）
//   4) 全文标黄（子串模式，上限 1000 处）+ 定位到命中行的第一个标黄处
#include "gui/viewer.h"
#include "core/encoding.h"
#include "core/matcher.h"
#include "core/types.h"
#include "core/util.h"
#include "core/logger.h"
#include "core/i18n.h"

#include <algorithm>
#include <climits>
#include <cwchar>
#include <string>
#include <vector>

#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER 0x0300
#endif
#include <richedit.h>
#include <commctrl.h>   // SetWindowSubclass / DefSubclassProc
#include <windowsx.h>

#ifndef EM_SETTARGETDEVICE
#define EM_SETTARGETDEVICE (WM_USER + 72)
#endif
#ifndef EM_GETFIRSTVISIBLELINE
#define EM_GETFIRSTVISIBLELINE (WM_USER + 20)
#endif
#ifndef EM_POSFROMCHAR
#define EM_POSFROMCHAR (WM_USER + 38)   // RichEdit 2.0+：wParam=字符偏移，返回坐标打包
#endif
#ifndef EM_GETLINECOUNT
#define EM_GETLINECOUNT (WM_USER + 48)
#endif
#ifndef EM_GETSCROLLPOS
#define EM_GETSCROLLPOS (WM_USER + 221)   // RichEdit 2.0+：读滚动位置（像素）
#endif
#ifndef EM_SETSCROLLPOS
#define EM_SETSCROLLPOS (WM_USER + 222)   // RichEdit 2.0+：设滚动位置（像素）
#endif
#ifndef EM_REQUESTRESIZE
#define EM_REQUESTRESIZE (WM_USER + 61)   // RichEdit：强制全量重排（刷新内部度量）
#endif
// 二次定位消息（发给查看器 RichEdit 自身；WM_APP+1..6 已被主窗口占用）
constexpr UINT WM_VIEWER_REPOSITION = WM_APP + 7;

#ifndef ES_DISABLENOSCROLL
#define ES_DISABLENOSCROLL 0x00002000L
#endif

namespace {

// RichEdit 控件类名（riched20.dll 提供）
const wchar_t* kRichEditClass = RICHEDIT_CLASSW;
// 行号槽窗口类名
const wchar_t* kGutterClass = L"TextSearchViewerGutter";

// 大文件阈值默认值（可在 ini [Preview] 覆盖，经 set_large_file_limits 注入）
constexpr size_t kLargeFileCharThreshold = 1ULL * 1024 * 1024;  // 1M wchar_t
constexpr int kLargeFileContextLines = 200;

// 行号槽左右留白（像素，96dpi 基准，随字体度量放大）
constexpr int kGutterPadRight = 8;
constexpr int kGutterPadLeft = 4;

// 编码枚举 → 可读标签（P-022）
std::wstring encoding_to_label(Encoding enc) {
    switch (enc) {
        case Encoding::Utf8:    return L"UTF-8";
        case Encoding::Gbk:     return L"GBK";
        case Encoding::Utf16Le: return L"UTF-16LE";
        case Encoding::Utf16Be: return L"UTF-16BE";
        case Encoding::Unknown:
        default:                return i18n::tr("view.enc_unknown");
    }
}

// 按宽字符 \n 切行（保留行内容，去除行尾 \r）
std::vector<std::wstring> split_lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n') {
            std::wstring line = text.substr(start, i - start);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(std::move(line));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        std::wstring line = text.substr(start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

// ---- 行号槽窗口过程 / RichEdit 子类化（Viewer 静态成员，转回私有实现）----

// ⚠️ 语义约定：自动换行仅是 RichEdit 的显示折行（EM_SETTARGETDEVICE 视觉重排），
// 文本内容与 \r\n 分隔完全不变；EM_GETLINECOUNT / EM_LINEINDEX / 行号编号全部
// 基于「逻辑段落 = 文件真实行」，折行产生的续行不显示行号——因此预览的行号
// 永远与文件真实行号一一对应，绝不因换行而增加。
LRESULT CALLBACK Viewer::gutter_proc(HWND hWnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam) {
    if (msg == WM_PAINT) {
        auto* self = reinterpret_cast<Viewer*>(
            GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (self) {
            self->paint_gutter();
            return 0;
        }
    } else if (msg == WM_ERASEBKGND) {
        return 1;   // 背景在 WM_PAINT 里一并填充，避免闪烁
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// RichEdit 子类化：重绘联动刷新行号槽 + 接收二次定位消息
LRESULT CALLBACK Viewer::richedit_subproc(HWND hWnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR /*id*/,
                                          DWORD_PTR refData) {
    auto* self = reinterpret_cast<Viewer*>(refData);
    if (msg == WM_VIEWER_REPOSITION) {
        if (self) self->scroll_to_hit();
        return 0;
    }
    LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
    if (msg == WM_PAINT) {
        if (self && self->gutter_) InvalidateRect(self->gutter_, nullptr, TRUE);
    }
    return r;
}

// 滚动定位：闭环反馈（见 viewer.h 注释）。
// 每轮：读目标字符的视口 y → 与期望位（上下文行）求差 → 按差修正滚动位。
// 「读数 - 修正」的差值在任意滚动状态下都成立，riched20 惰性失真会被
// 下一轮的新读数自纠；收敛（差 < 半行）或到内容边界（修正量不再变化）即止。
void Viewer::scroll_to_hit() {
    if (!hwnd_ || target_paragraph_ < 0) return;
    LRESULT line_char = SendMessageW(hwnd_, EM_LINEINDEX,
                                     static_cast<WPARAM>(target_paragraph_), 0);
    if (line_char < 0) return;
    long off = static_cast<long>(line_char) +
               static_cast<long>(target_within_);
    // 放置插入点（后续键盘翻页从这里继续）
    SendMessageW(hwnd_, EM_SETSEL, static_cast<WPARAM>(off),
                 static_cast<LPARAM>(off));

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int view_h = rc.bottom - rc.top;
    int row_h = 16;
    HDC dc = GetDC(hwnd_);
    if (dc) {
        HFONT f = reinterpret_cast<HFONT>(
            SendMessageW(hwnd_, WM_GETFONT, 0, 0));
        HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0)
            row_h = tm.tmHeight;
        if (old) SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
    }

    // 第一步：强制回顶（上滚到 0 永远合法、可靠落地；两连发 + 沉淀）。
    // ⚠️ doc swap 后视图可能带着上一文档的位移，必须先归零再测量。
    POINT zero{0, 0};
    SendMessageW(hwnd_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&zero));
    Sleep(30);
    SendMessageW(hwnd_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&zero));
    Sleep(30);

    // 第二步：基线 0（视口顶）下测内容总高。
    // ⚠️ 终因分流（2026-10-27）：riched20 允许 EM_SETSCROLLPOS 滚进「超出
    // 可滚区间」的位移——滚动条无滑块可表达，鼠标再也滚不回去（btshan1.c
    // 内容装得下视口、视图却被定位滚到第 8 行的孤儿状态）。装得下 → 全文
    // 可见、无需任何滚动，直接收工（目标行由黄块标记，必然在视口内）。
    int text_len = GetWindowTextLengthW(hwnd_);
    LONG content_h = 0;
    if (text_len > 0) {
        LRESULT lpos = SendMessageW(hwnd_, EM_POSFROMCHAR,
                                    static_cast<WPARAM>(text_len - 1), 0);
        int y_last = static_cast<short>(HIWORD(lpos));
        if (y_last >= 0) content_h = y_last + row_h;
    }
    LONG max_scroll = content_h - view_h;
    if (max_scroll < 0) {
        SendMessageW(hwnd_, EM_SETSEL, 0, 0);   // 全文可见，收工
        if (gutter_) InvalidateRect(gutter_, nullptr, TRUE);
        return;
    }

    // 第三步：内容超出视口 → 闭环反馈定位（滑块存在，无孤儿风险）；
    // 上下文边距与滚动量全程钳制在真实可滚区间 [0, max_scroll] 内。
    int margin = context_lines_ * row_h;
    int max_margin = view_h / 3;
    if (margin > max_margin) margin = max_margin;
    if (margin < 0) margin = 0;

    // 自记账本：不读 EM_GETSCROLLPOS（惰性失真会诱发提前收敛）——把每轮
    // 观测到的视口差累加到「我最后一次设定的绝对位」上再设定；观测滞后
    // 由下一轮的新读数自纠。
    LONG applied = 0;
    for (int iter = 0; iter < 12; ++iter) {
        LRESULT pos = SendMessageW(hwnd_, EM_POSFROMCHAR,
                                   static_cast<WPARAM>(off), 0);
        int y = static_cast<short>(HIWORD(pos));
        if (y < 0) break;   // 测不到（防御）
        int err = y - margin;
        if (err > -row_h / 2 && err < row_h / 2) break;   // 已就位
        LONG new_applied = applied + err;
        if (new_applied < 0) new_applied = 0;
        if (new_applied > max_scroll) new_applied = max_scroll;
        if (new_applied == applied) break;   // 已在内容边界
        applied = new_applied;
        POINT set{0, applied};
        SendMessageW(hwnd_, EM_SETSCROLLPOS, 0,
                     reinterpret_cast<LPARAM>(&set));
        Sleep(30);   // 给惰性重排一点沉淀，下一轮读数更真
    }
    SendMessageW(hwnd_, EM_SETSEL, 0, 0);   // 清选区（视图已就位）
    if (gutter_) InvalidateRect(gutter_, nullptr, TRUE);
}

void Viewer::request_reposition() {
    if (content_loaded_ && target_paragraph_ >= 0 && hwnd_) {
        PostMessageW(hwnd_, WM_VIEWER_REPOSITION, 0, 0);
    }
}

Viewer::Viewer() = default;
Viewer::~Viewer() {
    // 控件由父窗口销毁时自动回收，无需显式 DestroyWindow
    if (wrap_dc_ && hwnd_) ReleaseDC(hwnd_, wrap_dc_);
    wrap_dc_ = nullptr;
}

bool Viewer::create(HWND parent, HINSTANCE /*hInst*/) {
    // RichEdit 控件类由 riched20.dll 在加载时注册，必须先显式加载该 DLL，
    // 否则 CreateWindowExW(RICHEDIT_CLASSW) 会因类未注册而失败。
    static HMODULE s_riched = LoadLibraryW(L"riched20.dll");
    if (!s_riched) {
        logger::error(L"加载 riched20.dll 失败，无法创建查看器");
        return false;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        kRichEditClass,
        L"",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_WANTRETURN |
            ES_DISABLENOSCROLL |   // 滚动条常显：内容装得下时置灰而非消失——
                                   // 否则短文件在最大化预览里滚动条忽隐忽现，
                                   // 否则短文件下滚动条忽隐忽现，用户以为丢功能
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
        0, 0, 100, 100,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd_) {
        logger::error(L"创建 RichEdit 查看器失败");
        return false;
    }

    // 占位字体：主窗口创建后会立即用全界面同款字体（微软雅黑 UI）覆盖，
    // 这里只保证控件在极早期也有可用字体。
    HFONT hFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), FALSE);

    // 关闭自动 URL 检测等，保持纯文本
    SendMessageW(hwnd_, EM_AUTOURLDETECT, FALSE, 0);
    // 禁用编辑（只读已由 ES_READONLY 保证）
    SendMessageW(hwnd_, EM_SETREADONLY, TRUE, 0);

    // ---- 行号槽（自绘子窗口，RichEdit 左侧）----
    static bool gutter_class_registered = false;
    if (!gutter_class_registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &Viewer::gutter_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;   // 自绘
        wc.lpszClassName = kGutterClass;
        RegisterClassW(&wc);
        gutter_class_registered = true;
    }
    gutter_ = CreateWindowExW(0, kGutterClass, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 10, 10, parent, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (gutter_) {
        SetWindowLongPtrW(gutter_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        // 子类化 RichEdit：refData 携带 Viewer*（重绘联动行号槽 + 二次定位）
        SetWindowSubclass(hwnd_, &Viewer::richedit_subproc, 2,
                          reinterpret_cast<DWORD_PTR>(this));
    }

    update_gutter_width();   // 建立初始槽宽（无内容时两位数宽）
    set_word_wrap(word_wrap_);   // 应用初始换行态（默认开）
    return true;
}

void Viewer::resize(int x, int y, int w, int h) {
    last_rect_ = {x, y, x + w, y + h};
    layout_children();
}

void Viewer::layout_children() {
    int x = last_rect_.left, y = last_rect_.top;
    int w = last_rect_.right - last_rect_.left;
    int h = last_rect_.bottom - last_rect_.top;
    if (h <= 0 || w <= 0) return;
    if (gutter_ && gutter_w_ > 0)
        MoveWindow(gutter_, x, y, gutter_w_, h, TRUE);
    if (hwnd_)
        MoveWindow(hwnd_, x + gutter_w_, y, w - gutter_w_, h, TRUE);
}

void Viewer::set_word_wrap(bool on) {
    word_wrap_ = on;
    if (!hwnd_) return;
    SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);   // 切换期间确保可重绘
    apply_wrap_target();
    // 折行切换的重排同样可能延迟到绘制——先同步消化，再回到定位处，
    // 否则重排会把视图顶回文件头。
    UpdateWindow(hwnd_);
    // 定位 = 闭环反馈（对任意当前滚动状态成立），随后再投一次校准
    if (content_loaded_) scroll_to_hit();
    request_reposition();
    if (gutter_) InvalidateRect(gutter_, nullptr, TRUE);
}

void Viewer::apply_wrap_target() {
    if (!hwnd_) return;
    if (word_wrap_) {
        // 释放关闭换行时缓存的度量 DC
        if (wrap_dc_) { ReleaseDC(hwnd_, wrap_dc_); wrap_dc_ = nullptr; }
        // 折行到窗口宽度（hdc=NULL, 宽=0）
        SendMessageW(hwnd_, EM_SETTARGETDEVICE, 0, 0);
    } else {
        // 不折行：给一个超宽目标设备。RichEdit 会持有该 DC 做度量，
        // 故缓存为成员（析构/切回时释放），不能用即取即放的 GetDC/ReleaseDC。
        if (!wrap_dc_) wrap_dc_ = GetDC(hwnd_);
        SendMessageW(hwnd_, EM_SETTARGETDEVICE,
                     reinterpret_cast<WPARAM>(wrap_dc_), 0x7FFFFFF);
    }
}

void Viewer::clear() {
    if (hwnd_) {
        SetWindowTextW(hwnd_, L"");
    }
    enc_label_.clear();
    file_label_.clear();
    last_error_.clear();   // S8：清空预览的同时清掉上一次的失败原因
    content_loaded_ = false;
    hit_count_ = 0;
    highlights_capped_ = false;
    marks_.clear();
    target_paragraph_ = -1;
    target_within_ = 0;
    first_numbered_paragraph_ = 0;
    first_line_number_ = 1;
    update_gutter_width();
    if (gutter_) InvalidateRect(gutter_, nullptr, TRUE);
}

// ---- 标黄与定位 ----
// ⚠️ 坐标系约定（2026-10-26 修复）：RichEdit 内部把 \r\n 归一为**单个**段落符，
// display 字符串的字符偏移与控件偏移在每个换行处差 1、逐行累积。因此标黄与
// 定位一律用「(段落号, 行内偏移)」，应用时经 EM_LINEINDEX(段落) 换算控件真实
// 偏移——旧实现直接用 display 偏移，导致「标黄文字与搜索词不匹配」（越靠下
// 的行偏得越远）。

void Viewer::collect_substring_marks(const std::vector<std::wstring>& lines,
                                     size_t begin, size_t end,
                                     int first_paragraph,
                                     const std::wstring& pattern,
                                     bool ignore_case) {
    marks_.clear();
    highlights_capped_ = false;
    if (pattern.empty() || begin >= end) return;

    const bool need_target = (target_paragraph_ >= 0);
    std::wstring needle = ignore_case ? to_lower_wstring(pattern) : pattern;

    // 逐行收集 (段落号, 行内偏移)。收满 max_highlights_ 后仍继续扫到「定位行」
    // 为止——命中行在几百万处标黄之后时，也要拿到该行第一处作定位目标。
    bool past_target_line = false;
    for (size_t i = begin; i < end; ++i) {
        int par = first_paragraph + static_cast<int>(i - begin);
        bool is_target_line = need_target && !past_target_line &&
                              par == target_paragraph_;
        if (past_target_line && marks_.size() >= max_highlights_) break;
        if (marks_.size() >= max_highlights_ && !is_target_line && !need_target)
            break;

        const std::wstring& line = lines[i];
        const wchar_t* hay = nullptr;
        std::wstring lowered;
        if (ignore_case) {
            lowered = to_lower_wstring(line);
            hay = lowered.c_str();
        } else {
            hay = line.c_str();
        }
        size_t line_len = line.size();
        size_t pos = 0;
        bool first_on_line = true;
        while (pos + needle.size() <= line_len) {
            const wchar_t* found = std::wcsstr(hay + pos, needle.c_str());
            if (!found) break;
            size_t off = static_cast<size_t>(found - hay);
            bool take = (marks_.size() < max_highlights_) ||
                        (is_target_line && first_on_line);
            if (take) marks_.push_back({par, off, needle.size()});
            first_on_line = false;
            pos = off + needle.size();
            if (marks_.size() >= max_highlights_ && !is_target_line) break;
        }
        if (is_target_line) past_target_line = true;
    }
    // 截断判定：标黄数 < 全文命中数（受上限截断，或大文件仅显示部分行）
    highlights_capped_ = marks_.size() < hit_count_;
    if (marks_.size() > max_highlights_ + 1)
        marks_.resize(max_highlights_ + 1);   // 防御：最多「上限 + 定位行一处」
}

void Viewer::apply_highlights() {
    if (!hwnd_) return;
    for (const auto& m : marks_) {
        LRESULT line_char = SendMessageW(hwnd_, EM_LINEINDEX, m.paragraph, 0);
        if (line_char < 0) continue;
        long off = static_cast<long>(line_char) +
                   static_cast<long>(m.within);
        if (off < 0) continue;
        SendMessageW(hwnd_, EM_SETSEL, static_cast<WPARAM>(off),
                     static_cast<LPARAM>(off + m.len));
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_BACKCOLOR | CFM_COLOR;
        cf.crBackColor = RGB(255, 255, 0);   // 黄色背景（F.1.6）
        cf.crTextColor = RGB(0, 0, 0);        // 黑色文字（F.1.6）
        SendMessageW(hwnd_, EM_SETCHARFORMAT, SCF_SELECTION,
                     reinterpret_cast<LPARAM>(&cf));
    }
    SendMessageW(hwnd_, EM_SETSEL, 0, 0);
}


// ---- 行号槽 ----

void Viewer::paint_gutter() {
    if (!gutter_) return;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(gutter_, &ps);
    if (!dc) return;

    RECT rc{};
    GetClientRect(gutter_, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(243, 244, 247));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    if (hwnd_ && content_loaded_) {
        HFONT f = reinterpret_cast<HFONT>(
            SendMessageW(hwnd_, WM_GETFONT, 0, 0));
        HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
        SetTextColor(dc, RGB(128, 130, 138));
        SetBkMode(dc, TRANSPARENT);
        SetTextAlign(dc, TA_RIGHT | TA_TOP);

        // 行高（空行回填用：空行不折行，恰占一标准行高）
        int row_h = 16;
        {
            TEXTMETRICW tm{};
            if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0)
                row_h = tm.tmHeight;
        }

        int total = static_cast<int>(
            SendMessageW(hwnd_, EM_GETLINECOUNT, 0, 0));
        int first = static_cast<int>(
            SendMessageW(hwnd_, EM_GETFIRSTVISIBLELINE, 0, 0));
        int par = (first > 1) ? first - 1 : 0;
        for (; par < total; ++par) {
            LRESULT ch = SendMessageW(hwnd_, EM_LINEINDEX, par, 0);
            if (ch < 0) break;
            LRESULT pos = SendMessageW(hwnd_, EM_POSFROMCHAR,
                                       static_cast<WPARAM>(ch), 0);
            int y = static_cast<short>(HIWORD(pos));
            if (y > rc.bottom) break;
            // 空行修正：EM_POSFROMCHAR 对空段落（段落符本身）可能返回无效
            // 坐标——用「下一个有效锚点的 y 反推」（空段恰占一标准行高）。
            // 空行也应有行号（与主流编辑器一致），不能静默跳过。
            if (y < 0) {
                LRESULT len = SendMessageW(hwnd_, EM_LINELENGTH,
                                           static_cast<WPARAM>(ch), 0);
                if (len != 0) continue;   // 非空行取不到坐标：跳过（防御）
                int q = par + 1;
                int yq = -1;
                while (q < total) {
                    LRESULT chq = SendMessageW(hwnd_, EM_LINEINDEX, q, 0);
                    if (chq < 0) break;
                    LRESULT posq = SendMessageW(hwnd_, EM_POSFROMCHAR,
                                                static_cast<WPARAM>(chq), 0);
                    int yy = static_cast<short>(HIWORD(posq));
                    if (yy >= 0 && yy <= rc.bottom + row_h) { yq = yy; break; }
                    if (yy > rc.bottom + row_h) break;
                    ++q;
                }
                if (yq >= 0) {
                    y = yq - (q - par) * row_h;
                } else {
                    continue;   // 其后无有效锚点（文档尾连续空行）：本轮不绘
                }
                if (y < 0) continue;
            }
            if (par >= first_numbered_paragraph_) {
                int num = par - first_numbered_paragraph_ + first_line_number_;
                wchar_t buf[24];
                swprintf_s(buf, L"%d", num);
                TextOutW(dc, rc.right - kGutterPadRight, y + 1, buf,
                         static_cast<int>(wcslen(buf)));
            }
        }
        if (old) SelectObject(dc, old);
    }
    EndPaint(gutter_, &ps);
}

void Viewer::update_gutter_width() {
    if (!gutter_) return;
    int max_num = first_line_number_;
    if (hwnd_ && content_loaded_) {
        int total = static_cast<int>(
            SendMessageW(hwnd_, EM_GETLINECOUNT, 0, 0));
        int numbered = total - first_numbered_paragraph_;
        if (numbered > 0)
            max_num = first_line_number_ + numbered - 1;
    }
    int digits = 1;
    for (int v = max_num > 0 ? max_num : 1; v >= 10; v /= 10) ++digits;
    if (digits < 2) digits = 2;   // 最小两位宽，避免载入瞬间宽度跳动

    HDC dc = GetDC(gutter_);
    int digit_w = 8;
    if (dc) {
        HFONT f = reinterpret_cast<HFONT>(
            SendMessageW(hwnd_ ? hwnd_ : gutter_, WM_GETFONT, 0, 0));
        HGDIOBJ old = f ? SelectObject(dc, f) : nullptr;
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, L"0", 1, &sz) && sz.cx > 0)
            digit_w = sz.cx;
        if (old) SelectObject(dc, old);
        ReleaseDC(gutter_, dc);
    }
    int w = digits * digit_w + kGutterPadLeft + kGutterPadRight;
    if (w != gutter_w_) {
        gutter_w_ = w;
        layout_children();
    }
}

// ---- 载入与显示 ----

void Viewer::show_file(const std::filesystem::path& path, int hit_line,
                       const std::wstring& pattern, bool ignore_case,
                       bool use_regex, bool whole_word) {
    if (!hwnd_) return;

    clear();

    // S8：先判存在性。索引里的结果可能指向已被删除/移动的文件，
    // 「文件为空」和「打不开」是两回事，旧版混成一句话会让用户误判。
    std::error_code ex_ec;
    if (!std::filesystem::exists(path, ex_ec)) {
        SetWindowTextW(hwnd_, i18n::tr("view.missing"));
        enc_label_ = L"—";
        file_label_ = path.filename().wstring();
        last_error_ = L"文件不存在（可能已被删除或移动，重建索引可清除）";
        logger::warn(L"查看器文件不存在: " + path.wstring());
        return;
    }

    // 大文件保护：超过 32MB 不在预览中全量载入，避免 OOM
    std::error_code sz_ec;
    auto fsize = std::filesystem::file_size(path, sz_ec);
    if (!sz_ec && fsize > large_file_bytes_) {
        SetWindowTextW(hwnd_, i18n::tr("view.too_big"));
        enc_label_ = L"—";
        file_label_ = path.filename().wstring();
        last_error_ = L"文件过大（>32MB），预览未载入";
        logger::warn(L"查看器跳过过大文件: " + path.wstring());
        return;
    }

    // 读取文件字节并识别编码（P-022）
    std::vector<unsigned char> bytes = read_file_bytes(path);
    if (bytes.empty()) {
        // S8：文件存在却读不到 → 只可能是权限不足或独占锁（0 字节另算）
        bool empty_file = (!sz_ec && fsize == 0);
        if (empty_file) {
            SetWindowTextW(hwnd_, i18n::tr("view.empty"));
            last_error_ = L"文件为空";
        } else {
            SetWindowTextW(hwnd_, i18n::tr("view.no_access"));
            last_error_ = L"无读取权限或文件被其他程序占用";
            logger::warn(L"查看器读取文件失败: " + path.wstring());
        }
        enc_label_ = L"—";
        file_label_ = path.filename().wstring();
        return;
    }

    Encoding enc = detect_encoding(bytes);
    enc_label_ = encoding_to_label(enc);
    file_label_ = path.filename().wstring();

    std::wstring text = decode_bytes(bytes, enc);

    // 命中计数（全文口径；与结果列表同一匹配引擎 search_in_text）。
    // 正则在 UI 线程上跑，带 5s 计数限时——病态 pattern 顶多少计，不能卡死界面。
    if (!pattern.empty() && !text.empty()) {
        int flags = MatchFlags::Substring;
        if (use_regex) flags |= MatchFlags::Regex;
        if (ignore_case) flags |= MatchFlags::IgnoreCase;
        if (whole_word) flags |= MatchFlags::WholeWord;
        double timeout = use_regex ? 5.0 : 0.0;
        auto hits = search_in_text(text, pattern, flags, path, 100000, timeout);
        hit_count_ = hits.size();
    }

    // 决定显示范围：小文件全量显示，大文件只显示命中行附近窗口
    bool use_window = (text.size() > kLargeFileCharThreshold);

    std::vector<std::wstring> lines = split_lines(text);

    std::wstring display;
    int hit_in_display = hit_line;   // 命中行在显示内容中的行号（1 起，含提示行）
    size_t slice_begin = 0, slice_end = lines.size();
    int first_paragraph = 0;

    if (use_window) {
        // 大文件：只显示命中行附近 kLargeFileContextLines 行
        int total = static_cast<int>(lines.size());
        int ctx = large_file_ctx_;
        int start_line = (std::max)(1, hit_line - ctx);
        int end_line = (std::min)(total, hit_line + ctx);

        // 正则模式不做字面量高亮（R20），提示语相应区分
        std::wstring note =
            (use_regex || pattern.empty()
                 ? i18n::trf("view.window_note_no",
                             std::to_wstring(start_line),
                             std::to_wstring(end_line))
                 : i18n::trf("view.window_note",
                             std::to_wstring(start_line),
                             std::to_wstring(end_line)));
        display = note;
        hit_in_display = 1 + (hit_line - start_line);
        slice_begin = static_cast<size_t>(start_line - 1);
        slice_end = static_cast<size_t>(end_line);
        first_paragraph = 1;   // 提示行占段落 0，内容从段落 1 起

        for (size_t i = slice_begin; i < slice_end; ++i) {
            display += lines[i];
            display += L"\r\n";
        }
        // 行号：跳过提示段落，从 start_line 起编
        first_numbered_paragraph_ = 1;
        first_line_number_ = start_line;
    } else {
        // 小文件：全量显示
        for (const auto& ln : lines) {
            display += ln;
            display += L"\r\n";
        }
        hit_in_display = hit_line;
        first_numbered_paragraph_ = 0;
        first_line_number_ = 1;
    }

    // 定位目标：命中行段落（行内偏移先置 0，标黄收集后精确化为首个标黄处）
    if (hit_in_display >= 1) {
        target_paragraph_ = hit_in_display - 1;
        target_within_ = 0;
    }

    // 设置文本。折行目标设备的重发与滚动统一放到收尾处（见函数末尾注释）——
    // 在 WM_SETREDRAW FALSE 期间发送 EM_SETTARGETDEVICE，其重排会被推迟，
    // 与滚动次序纠缠。
    SendMessageW(hwnd_, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(hwnd_, display.c_str());
    // 全文标黄（子串模式；R20 正则不标黄）。必须在定位前完成——
    // 定位目标是「命中行上的第一个标黄处」。
    if (!use_regex && !pattern.empty()) {
        collect_substring_marks(lines, slice_begin, slice_end,
                                first_paragraph, pattern, ignore_case);
        for (const auto& m : marks_) {
            if (m.paragraph == target_paragraph_) {
                target_within_ = m.within;
                break;
            }
        }
        apply_highlights();
    }

    content_loaded_ = true;
    update_gutter_width();   // 尺寸先定（含可能的 MoveWindow 引发的重排）

    // ⚠️ 滚动必须放在「重排完全消化 + 滚动状态清零」之后：换文档时
    // EM_GETSCROLLPOS/滚动条度量可能仍报上一文档的值（2026-10-27 探针实证，
    // riched20 惰性更新，WM_SIZE 也刷不动）→ EM_REQUESTRESIZE 强制重排 +
    // 滚动位清零双保险，定位处的钳制则完全自算（不依赖惰性度量）。
    SendMessageW(hwnd_, WM_SETREDRAW, TRUE, 0);
    apply_wrap_target();
    SendMessageW(hwnd_, EM_REQUESTRESIZE, 0, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    // 定位 = 闭环反馈（scroll_to_hit），随后再投一次（异步重排消化后校准）
    scroll_to_hit();
    request_reposition();
}
