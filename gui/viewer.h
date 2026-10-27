// gui/viewer.h — 右栏只读查看器（T5 5.4d）
// 选中/双击结果 → 载入文件 → 全文标黄 → 滚动定位到命中处。
// 命中关键词用黄色背景+黑色文字标记（附录 F.1.6）；显示命中处 ±N 行上下文
// （P-011，可配）。2026-10-26 增：自动换行（可切）、左侧行号槽、全文命中
// 计数（经 core/matcher 与搜索结果同一口径）、定位到首个标黄处。
// 编码信息通过 encoding_label() 暴露给主窗口在标题展示（P-022）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class Viewer {
public:
    Viewer();
    ~Viewer();

    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;

    // 创建 RichEdit 控件（只读）+ 左侧行号槽。parent 为右栏容器。
    bool create(HWND parent, HINSTANCE hInst);

    // 重新布局（内部把宽度切分为：行号槽 + RichEdit）。
    void resize(int x, int y, int w, int h);

    HWND hwnd() const { return hwnd_; }

    // 清空查看器。
    void clear();

    // 自动换行（RichEdit EM_SETTARGETDEVICE 切换；关闭时固定超宽目标设备
    // 模拟不折行）。切换后重新定位到上次命中处，避免重排后视图漂走。
    void set_word_wrap(bool on);
    bool word_wrap() const { return word_wrap_; }

    // 显示指定文件：全文统计命中数 + 全文标黄（子串模式，有上限）+
    // 滚动定位到「命中行上的第一个标黄处」（无标黄时定位到命中行行首）。
    // - path:        文件路径
    // - hit_line:    命中行号（1-indexed）
    // - pattern:     搜索模式（子串模式按字面量标黄；正则模式只计数不标黄，R20）
    // - ignore_case / use_regex / whole_word: 与本次搜索同口径的匹配选项
    void show_file(const std::filesystem::path& path, int hit_line,
                   const std::wstring& pattern, bool ignore_case,
                   bool use_regex, bool whole_word);

    // 最近一次显示的文件编码标签（如 L"GBK" / L"UTF-8"），供标题展示（P-022）。
    const std::wstring& encoding_label() const { return enc_label_; }
    const std::wstring& file_label() const { return file_label_; }

    // 本次显示的全文命中数（show_file 用 core/matcher 统计，口径同结果列表；
    // 正则模式带 5s 计数限时防 UI 卡死，超时时为部分计数）。pattern 为空时为 0。
    size_t hit_count() const { return hit_count_; }
    // 实际已标黄的位置数（≤ 上限；大文件窗口模式下仅为显示范围内的数量）。
    size_t highlight_count() const { return marks_.size(); }
    // 标黄是否被截断（命中数超过已标白数——受上限或大文件仅显示部分行所致；
    // 命中计数本身始终精确）。
    bool highlights_capped() const { return highlights_capped_; }

    // S8：最近一次 show_file 的失败原因，空串表示成功。
    const std::wstring& last_error() const { return last_error_; }

    // 悬停滚轮转发：判断主窗口客户区坐标是否落在预览区内
    // （含行号槽）。主窗口/两个列表用它决定是否把 WM_MOUSEWHEEL 转给预览。
    bool contains_point(POINT pt) const {
        return pt.x >= last_rect_.left && pt.x < last_rect_.right &&
               pt.y >= last_rect_.top && pt.y < last_rect_.bottom;
    }

    // 设置上下文行数（P-011，默认 3）。
    void set_context_lines(int n) { context_lines_ = n < 0 ? 0 : n; }

    // 可配置常量注入（ini [Preview]，MainWindow 启动时调用）：
    // 大文件预览阈值（字节）与上下文行数、标黄上限。
    void set_large_file_limits(size_t max_file_bytes, int ctx_lines) {
        large_file_bytes_ = max_file_bytes;
        large_file_ctx_ = ctx_lines < 1 ? 1 : ctx_lines;
    }
    void set_max_highlights(size_t n) { max_highlights_ = n < 1 ? 1 : n; }

    // 二次定位（异步重排吸收）：show_file/set_word_wrap 调用——riched20 的
    // 重排/基线清零可能晚于同步调用完成，向控件投递私有消息，由
    // reposition_step() 在消息循环里分轮完成「归零→测量→定位」。
    // 无内容/无定位目标时不投递。
    void request_reposition();

private:
    // 收集子串出现位置。⚠️ 偏移全部以「(段落号, 行内偏移)」记录，应用时经
    // EM_LINEINDEX 换算控件真实字符偏移——RichEdit 内部把 \r\n 归一为单个
    // 段落符，display 字符串坐标与控件坐标每个换行差 1，直接用必然漂移
    // （2026-10-26 修复：标黄文字与搜索词不匹配的根因）。
    void collect_substring_marks(const std::vector<std::wstring>& lines,
                                 size_t begin, size_t end,
                                 int first_paragraph,
                                 const std::wstring& pattern, bool ignore_case);
    // 应用收集到的标黄（上限 kMaxHighlights）。
    void apply_highlights();
    // 仅发送 EM_SETTARGETDEVICE（不滚动）。show_file 每次 SetWindowText 后
    // 必须重发——RichEdit 设文本会重置折行目标设备。
    void apply_wrap_target();
    // 滚动定位：闭环反馈（2026-10-27 五修）。
    // riched20 换文档/重排后一切同步测量（滚动位/度量/坐标）都可能惰性
    // 失真——开环方案（像素/行基/状态机）都曾被异步重排打偏。改为闭环：
    // 反复读「目标字符当前视口 y」与期望位（上下文行）的差，按差修正滚动
    // 位，再读再修——差值在任意滚动状态下都成立，惰性失真被下一轮新读数
    // 自纠；收敛或到达内容边界即止。不依赖任何度量查询与基线假设。
    void scroll_to_hit();
    // 行号槽重绘（RichEdit 每次重绘时联动调用）。
    void paint_gutter();
    // 依据当前内容重算行号槽宽度并重排子控件。
    void update_gutter_width();
    void layout_children();

    // 行号槽窗口过程 / RichEdit 子类化过程（需访问私有 paint_gutter）
    static LRESULT CALLBACK gutter_proc(HWND hWnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam);
    static LRESULT CALLBACK richedit_subproc(HWND hWnd, UINT msg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR id,
                                             DWORD_PTR refData);

    HWND hwnd_ = nullptr;            // RichEdit 控件句柄
    HWND gutter_ = nullptr;          // 左侧行号槽（自绘子窗口）
    int gutter_w_ = 0;               // 当前行号槽宽度（像素）
    RECT last_rect_{};               // 最近一次 resize 的区域
    HDC wrap_dc_ = nullptr;          // 关闭换行时缓存的度量 DC（RichEdit 持有引用）

    bool word_wrap_ = true;
    bool content_loaded_ = false;    // 是否有已载入内容（行号/定位仅此时有意义）
    int first_numbered_paragraph_ = 0;  // 行号从该段落起编号（跳过大文件提示行）
    int first_line_number_ = 1;         // 该段落对应的文件行号
    // 定位目标：段落号 + 行内偏移（控件真实坐标经 EM_LINEINDEX 换算）
    int target_paragraph_ = -1;
    size_t target_within_ = 0;

    size_t hit_count_ = 0;
    bool highlights_capped_ = false;
    size_t max_highlights_ = 1000;   // 标黄上限（ini [Preview] max_highlights）
    // 标黄位置：(段落号, 行内偏移, 长度)——段落号与行内偏移与控件坐标一一对应
    struct Mark { int paragraph; size_t within; size_t len; };
    std::vector<Mark> marks_;

    std::wstring enc_label_;         // 编码标签
    std::wstring file_label_;        // 文件名标签
    std::wstring last_error_;        // S8：上次显示失败的原因（空=成功）
    int context_lines_ = 3;          // 上下文行数（P-011）
    size_t large_file_bytes_ = 32ULL * 1024 * 1024;  // 大文件预览阈值（字节）
    int large_file_ctx_ = 200;       // 大文件预览上下文行数
};
