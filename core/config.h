// core/config.h — 配置读写接口（附录 F.1.5 扩展）
#pragma once

#include <filesystem>
#include <string>

// 应用配置（settings.json 持久化）
struct AppConfig {
    // 搜索参数
    size_t threshold_mb = 50;            // 大文件阈值
    std::wstring last_root;             // 上次搜索路径
    // 注：不再有 last_pattern 字段——关键词（可能是密码/API Key）不落盘，
    // 仅存于内存供本次会话的高亮/导出使用（B2）。
    bool recursive = true;              // 递归
    bool use_regex = false;            // 正则模式
    bool ignore_case = false;          // 忽略大小写
    bool whole_word = false;           // 全词匹配
    bool force_realtime = false;       // 强制即时扫描（跳过索引，用于两种模式结果比对）
    bool auto_update_index = true;     // 索引模式搜索前自动增量更新（防结果陈旧）

    // 窗口位置/大小（F.1.5）
    int window_x = 0;
    int window_y = 0;
    int window_w = 1200;
    int window_h = 800;
    bool window_maximized = false;

    // 三栏列宽（像素，0=使用默认值）
    int left_col_w = 0;
    int right_col_w = 0;

    // O5：结果列表三列宽度（像素，0=使用默认值）。命中行很长时用户会手动拖宽，
    // 不记住的话每次重启都要重拖一遍。
    int result_col0_w = 0;   // 文件路径
    int result_col1_w = 0;   // 行号
    int result_col2_w = 0;   // 命中行文本

    // O3：选项条折叠状态（此前每次启动都回到展开）
    bool options_collapsed = false;

    // 预览查看器：自动换行（默认开；界面「自动换行」复选框可切，退出时记忆）
    bool preview_word_wrap = true;

    // 界面语言（"system"/"zh"/"en"；切换后重启生效，见 core/i18n）
    std::wstring ui_lang = L"system";

    // ---- 可配置常量（原硬编码，2026-10-27 入 ini）----
    size_t max_results = 100000;        // [Search] 结果上限
    int    search_timeout_sec = 120;    // [Search] 单文件正则超时
    int    watchdog_sec = 60;           // [Search] 看门狗无进度阈值
    int    history_max = 10;            // [Search] 关键词历史条数
    int    preview_context_lines = 3;   // [Preview] 预览定位上下文行数
    int    preview_max_highlights = 1000; // [Preview] 标黄上限
    int    preview_large_file_mb = 32;  // [Preview] 预览大文件阈值
    int    preview_large_file_ctx = 200;// [Preview] 大文件预览上下文行数

    // 外观
    bool dark = false;

    // ---- B2 日志隐私（见 core/logger.h 的「B2 隐私开关」）----
    // 关键词从不落盘（无开关，硬保证）；此处只管日志本身与路径脱敏。
    bool log_enabled = true;         // 日志总开关
    bool log_redact_paths = true;    // 路径脱敏（默认开 → 安全默认）
};

// 读取配置（文件不存在返回默认值）
AppConfig load_config();

// 保存配置
bool save_config(const AppConfig& cfg);

// 指定配置文件的读写（便携模式 / 单测用）。
// 与上面两个同名重载的行为差别：带路径版本不做旧版 settings.json 迁移——
// 迁移只针对默认位置那一份。
AppConfig load_config(const std::filesystem::path& ini_path);
bool save_config(const AppConfig& cfg, const std::filesystem::path& ini_path);

// settings.json 路径
std::filesystem::path settings_path();
