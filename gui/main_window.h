// gui/main_window.h — 主窗口（T5 GUI 主界面 + T6 主流程整合）
// 宽屏三栏布局：顶部输入条 + 可折叠选项条 + 左(扩展名)/中(结果虚拟列表)/右(查看器) + 底部状态栏。
// 线程模型：主线程跑消息循环，worker 线程跑 Scanner，通过 PostMessage 回传进度/结果/完成。
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/scanner.h"
#include "core/types.h"
#include "core/config.h"
#include "core/extension_manager.h"
#include "gui/viewer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // 创建并显示主窗口。返回主窗口句柄；失败返回 nullptr。
    HWND create(HINSTANCE hInst, int nCmdShow);

    HWND hwnd() const { return hwnd_; }
    HACCEL accel_table() const { return hAccel_; }

private:
    // ---- 窗口过程 ----
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT dispatch(UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- 控件创建 / 布局 ----
    void on_create();
    void create_controls();
    void layout_controls();
    int scale_dpi(int v) const;

    // ---- 状态机 ----
    enum class State { Idle, Scanning };
    void set_state(State s);

    // ---- 搜索流程 ----
    void on_search();
    void on_cancel();
    void on_browse();
    void on_collect_extensions();
    void on_export_results();
    void on_build_index();

    // ---- 结果列表 ----
    void on_result_activate();        // 双击/Enter → 查看器
    void push_keyword_history(const std::wstring& kw);  // S5
    void show_keyword_history();                        // S5：弹出历史下拉
    void copy_selected_result();      // 「路径:行号\t内容」
    void copy_result_field(int which);   // S7：0=完整路径 1=行号
    void open_containing_folder();       // S7：资源管理器定位并选中该文件
    void open_in_editor();               // 默认工具打开；无关联回退记事本
    void show_result_context_menu(POINT pt, bool has_sel);  // S7：右键菜单
    int  selected_result_index() const;

    // ---- 自定义消息处理 ----
    void on_progress(size_t done, size_t total);
    void on_results(std::vector<MatchResult>* results);
    void on_skip(size_t skipped);
    void on_done(int status, std::wstring* msg);

    // ---- 状态栏 / 进度 ----
    void set_status(const std::wstring& text);
    DWORD list_view_custom_draw(LPARAM lParam);   // 结果/扩展名列表自绘（含 S3 取消置灰）
    void apply_sort();                            // S1：按 sort_col_/sort_asc_ 重排 results_
    void update_progress(size_t done, size_t total);
    void update_export_button();      // S9：无结果时禁用导出按钮
    void update_result_label();       // O9：结果栏标题（含「已达上限」提示）
    // 刷新状态栏「索引：已建立/未建立/—」pane（读取当前路径，调用 core Indexer::index_exists）
    void update_index_status();

    // ---- 选项条 ----
    void toggle_options();

    // ---- 选项读取 ----
    SearchOptions build_options();
    std::vector<std::wstring> parse_list_field(HWND hEdit) const;
    std::set<std::wstring> resolve_extensions();

    // ---- 配置持久化 ----
    void load_settings();
    void save_settings();

    // ---- 快捷键 ----
    void create_accelerators();
    void focus_column(int which);   // 1=左 2=中 3=右

    // ---- 看门狗（F.2.4）----
    void on_watchdog();

    // ---- 悬停滚轮转发 ----
    // 焦点在结果/扩展名列表（或主窗口冒泡）时，光标若悬停在预览区上，
    // 滚轮转发给查看器——预览是只读控件、从不主动获焦，不转发则永远滚不动。
    static LRESULT CALLBACK list_wheel_proc(HWND hWnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam,
                                            UINT_PTR idSubclass,
                                            DWORD_PTR refData);

    // ---- 行号列「命中行/文件总行」（2026-10-27 用户需求）----
    // 总行数按文件懒计算 + 缓存（流式计数，内存有界）；列宽固定 10 字符
    // （用户确认口径，覆盖「99999/9999」级常规情况），不走自适应——
    // 首版的自适应在「绘制回调置脏、搜索回调消费」的时序下浏览期永远
    // 不生效（「82/2...」截断根因）。
    int  file_line_total(const std::filesystem::path& p);
    int  lineno_col_fixed_width();   // 行号列固定宽（文本度量；load_settings 钳制用）
    std::unordered_map<std::wstring, int> file_total_lines_;

    // ---- 分隔条 ----
    int  hit_test_splitter(int x, int y) const;  // 0=none, 1=left/mid, 2=mid/right
    void start_drag_splitter(int which, int x);
    void on_drag_splitter(int x);
    void end_drag_splitter();

    // ---- 成员 ----
    HWND hwnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    HACCEL hAccel_ = nullptr;
    int dpi_ = 96;
    HFONT hFont_ = nullptr;            // 全局 UI 字体（微软雅黑 9pt）

    // 顶部输入条控件
    HWND hPathEdit_ = nullptr;
    HWND hBrowseBtn_ = nullptr;
    HWND hKeywordEdit_ = nullptr;
    HWND hHistoryBtn_ = nullptr;    // S5：关键词历史下拉按钮
    HWND hSearchBtn_ = nullptr;
    HWND hCancelBtn_ = nullptr;
    HWND hCollectBtn_ = nullptr;
    HWND hExportBtn_ = nullptr;
    HWND hIndexBtn_ = nullptr;
    HWND hSep1_ = nullptr;          // 顶部输入条分组竖直分隔线（G1|G2）
    HWND hSep2_ = nullptr;          // 顶部输入条分组竖直分隔线（G2|G3）
    // 顶部输入条标签（布局需要）
    HWND hLblPath_ = nullptr;
    HWND hLblKeyword_ = nullptr;

    // 选项条控件
    HWND hOptToggle_ = nullptr;
    HWND hChkRecursive_ = nullptr;
    HWND hChkRegex_ = nullptr;
    HWND hChkCase_ = nullptr;       // 区分大小写
    HWND hChkWholeWord_ = nullptr;
    HWND hChkRealtime_ = nullptr;    // 强制即时扫描（跳过索引，用于索引/实时结果比对）
    HWND hThresholdEdit_ = nullptr;
    HWND hWildcardEdit_ = nullptr;
    HWND hBlacklistEdit_ = nullptr;
    // 选项条标签
    HWND hLblThreshold_ = nullptr;
    HWND hLblThresholdMb_ = nullptr;
    HWND hLblWildcard_ = nullptr;
    HWND hLblBlacklist_ = nullptr;
    HWND hLblLang_ = nullptr;       // 语言选择（重启生效）
    HWND hLangCombo_ = nullptr;

    // 三栏控件
    HWND hExtList_ = nullptr;       // 左栏扩展名 ListView（带复选框）
    HWND hExtLabel_ = nullptr;
    HWND hResultList_ = nullptr;    // 中栏结果 ListView（虚拟/owner-data）
    HWND hResultLabel_ = nullptr;   // 中栏标题（与左/右栏对齐）
    HWND hViewerLabel_ = nullptr;   // 右栏标题（编码信息）
    HWND hWrapChk_ = nullptr;       // 预览「自动换行」复选框（纯显示折行，不改行号）
    Viewer viewer_;                 // 右栏查看器

    // 底部状态栏
    HWND hStatusBar_ = nullptr;
    HWND hProgressBar_ = nullptr;

    // 状态
    State state_ = State::Idle;
    std::vector<MatchResult> results_;   // 全部命中结果（虚拟列表数据源）
    std::shared_ptr<Scanner> scanner_;
    std::thread worker_thread_;
    std::atomic<bool> quitting_{false};
    bool user_cancelled_ = false;
    // S3.1：建索引流程的取消位（建库无 Scanner 可 cancel）。
    // on_cancel 在 Scanning 且 scanner_==nullptr（即建库中）时置位；
    // build worker 开始前检查，命中则直接按「已取消」收口。
    std::atomic<bool> build_cancelled_{false};
    // S5.5：update_index_status 的分离线程生命周期信号。析构/WM_CLOSE 置
    // false；线程只捕获该 shared_ptr + HWND 值，不再触碰 this，避免
    // MainWindow 析构后 use-after-free。
    std::shared_ptr<std::atomic<bool>> ui_alive_{std::make_shared<std::atomic<bool>>(true)};
    bool results_partial_ = false;   // S3：本次结果是「取消后的部分结果」→ 结果列表置灰
    bool index_mode_ = false;        // S6：本次搜索走索引分支（无「扫到第几个文件」概念）
    bool results_capped_ = false;    // O9：本次结果触达上限被截断 → 结果栏标题提示

    // S1：结果排序（默认按路径；点击列头切换）
    int sort_col_ = 0;          // 0=文件路径 / 1=行号 / 2=命中行文本
    bool sort_asc_ = true;

    // S5：最近搜索关键词（最近在前，上限 10）。
    // 只存内存、进程退出即清空——与 B2 一致：关键词永不写入配置文件。
    std::vector<std::wstring> keyword_history_;

    // 搜索上下文
    std::wstring last_pattern_;
    std::wstring scan_notice_;   // 扫描期间的错误/警告通知（用于完成时展示）
    std::wstring search_mode_label_;  // 本次搜索实际模式「（索引模式）/（实时扫描）」，建索引期间为空

    // 索引状态检测：单调递增的「代际号」。每次启动新检测 +1，异步回调比较
    // 代际号，旧请求的结果被忽略（避免用户切路径时旧检测覆盖新结果）。
    std::atomic<uint64_t> index_check_gen_{0};
    size_t skipped_count_ = 0;
    size_t done_count_ = 0;
    size_t total_count_ = 0;

    // 扩展名
    ExtensionManager ext_mgr_;
    std::set<std::wstring> checked_exts_;   // 收集对话框勾选的扩展名

    // 配置
    AppConfig config_;
    bool options_expanded_ = true;

    // 三栏列宽（像素，DPI 缩放后的实际值）
    int left_col_w_ = 0;    // 0=未初始化，layout_controls 中赋默认值
    int right_col_w_ = 0;
    int splitter_w_ = 0;    // 分隔条宽度（像素）

    // 分隔条拖拽状态
    int dragging_splitter_ = 0;  // 0=无, 1=左/中分隔, 2=中/右分隔
    int drag_anchor_x_ = 0;      // 拖拽起始 x 坐标
    int drag_anchor_w_ = 0;      // 拖拽起始列宽

    // 看门狗
    UINT_PTR timer_id_ = 0;
    ULONGLONG last_progress_tick_ = 0;

    // 关键词编辑框子类化旧过程
    WNDPROC keyword_old_proc_ = nullptr;
};
