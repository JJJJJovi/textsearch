// core/config.cpp — 配置读写实现（INI 格式）
//
// 配置文件：%LOCALAPPDATA%/TextSearch/textsearch.ini
// 旧版本使用 settings.json，首次启动时自动迁移到 ini（迁移后保留 json 文件不动）。
#include "core/config.h"
#include "core/inifile.h"
#include "core/json.h"
#include "core/util.h"

#include <fstream>

std::filesystem::path settings_path() {
    return get_app_data_dir() / L"textsearch.ini";
}

namespace {

// 旧版配置文件路径（用于一次性迁移）
std::filesystem::path legacy_settings_path() {
    return get_app_data_dir() / L"settings.json";
}

// 从旧 settings.json 迁移（若 ini 不存在而 json 存在）
bool migrate_from_json(IniFile& ini) {
    std::ifstream ifs(legacy_settings_path(), std::ios::binary);
    if (!ifs) return false;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    try {
        json::Value v = json::parse(content);
        auto rd_int = [&](const char* k, int def) -> int {
            if (const auto* p = v.find(k)) return p->as_int();
            return def;
        };
        auto rd_bool = [&](const char* k, bool def) -> bool {
            if (const auto* p = v.find(k)) return p->as_bool();
            return def;
        };
        auto rd_str = [&](const char* k) -> std::wstring {
            if (const auto* p = v.find(k)) return utf8_to_wstring(p->as_string());
            return L"";
        };
        ini.set_int(L"Search", L"threshold_mb", rd_int("threshold_mb", 50));
        ini.set(L"Search", L"last_root", rd_str("last_root"));
        // B2：last_pattern 不迁移——旧 JSON 里的关键词就此丢弃，不进新 ini
        ini.set_bool(L"Search", L"recursive", rd_bool("recursive", true));
        ini.set_bool(L"Search", L"use_regex", rd_bool("use_regex", false));
        ini.set_bool(L"Search", L"ignore_case", rd_bool("ignore_case", false));
        ini.set_bool(L"Search", L"whole_word", rd_bool("whole_word", false));
        ini.set_bool(L"Search", L"auto_update_index", rd_bool("auto_update_index", true));
        ini.set_int(L"Window", L"x", rd_int("window_x", 0));
        ini.set_int(L"Window", L"y", rd_int("window_y", 0));
        ini.set_int(L"Window", L"width", rd_int("window_w", 1200));
        ini.set_int(L"Window", L"height", rd_int("window_h", 800));
        ini.set_bool(L"Window", L"maximized", rd_bool("window_maximized", false));
        ini.set_int(L"Window", L"left_col_w", rd_int("left_col_w", 0));
        ini.set_int(L"Window", L"right_col_w", rd_int("right_col_w", 0));
        ini.set_bool(L"Window", L"dark", rd_bool("dark", false));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

// ini → AppConfig（两份 load_config 共用）
static AppConfig parse_config(const IniFile& ini) {
    AppConfig cfg;

    cfg.threshold_mb = static_cast<size_t>(ini.get_int(L"Search", L"threshold_mb", 50));
    cfg.last_root = ini.get(L"Search", L"last_root");
    // B2：不读取 last_pattern（搜索关键词可能是密码/API Key）。
    // 旧版本写进 ini 的那份会在 save_config 时被主动清除。
    cfg.recursive = ini.get_bool(L"Search", L"recursive", true);
    cfg.use_regex = ini.get_bool(L"Search", L"use_regex", false);
    cfg.ignore_case = ini.get_bool(L"Search", L"ignore_case", false);
    cfg.whole_word = ini.get_bool(L"Search", L"whole_word", false);
    cfg.force_realtime = ini.get_bool(L"Search", L"force_realtime", false);
    cfg.auto_update_index = ini.get_bool(L"Search", L"auto_update_index", true);

    cfg.window_x = ini.get_int(L"Window", L"x", 0);
    cfg.window_y = ini.get_int(L"Window", L"y", 0);
    cfg.window_w = ini.get_int(L"Window", L"width", 1200);
    cfg.window_h = ini.get_int(L"Window", L"height", 800);
    cfg.window_maximized = ini.get_bool(L"Window", L"maximized", false);
    cfg.left_col_w = ini.get_int(L"Window", L"left_col_w", 0);
    cfg.right_col_w = ini.get_int(L"Window", L"right_col_w", 0);
    // O5：结果列表列宽（缺省 0 → 由布局代码给默认值）
    cfg.result_col0_w = ini.get_int(L"Window", L"result_col0_w", 0);
    cfg.result_col1_w = ini.get_int(L"Window", L"result_col1_w", 0);
    cfg.result_col2_w = ini.get_int(L"Window", L"result_col2_w", 0);
    // O3：选项条折叠状态
    cfg.options_collapsed = ini.get_bool(L"Window", L"opt_collapsed", false);
    // 预览自动换行（默认开）
    cfg.preview_word_wrap = ini.get_bool(L"Window", L"preview_word_wrap", true);
    cfg.ui_lang = ini.get(L"Window", L"ui_lang");
    // 可配置常量（原硬编码）
    cfg.max_results = static_cast<size_t>(
        ini.get_int(L"Search", L"max_results", 100000));
    cfg.search_timeout_sec = ini.get_int(L"Search", L"search_timeout_sec", 120);
    cfg.watchdog_sec = ini.get_int(L"Search", L"watchdog_sec", 60);
    cfg.history_max = ini.get_int(L"Search", L"history_max", 10);
    cfg.preview_context_lines = ini.get_int(L"Preview", L"context_lines", 3);
    cfg.preview_max_highlights = ini.get_int(L"Preview", L"max_highlights", 1000);
    cfg.preview_large_file_mb = ini.get_int(L"Preview", L"large_file_mb", 32);
    cfg.preview_large_file_ctx = ini.get_int(L"Preview", L"large_file_ctx", 200);
    cfg.dark = ini.get_bool(L"Window", L"dark", false);

    // B2：日志隐私开关（默认全开保护，用户可显式关闭）
    cfg.log_enabled = ini.get_bool(L"Privacy", L"log_enabled", true);
    cfg.log_redact_paths = ini.get_bool(L"Privacy", L"log_redact_paths", true);

    return cfg;
}

AppConfig load_config() {
    IniFile ini;
    bool loaded = ini.load(settings_path());

    if (!loaded || ini.empty()) {
        // ini 不存在或为空 → 尝试从旧 json 迁移
        IniFile mig;
        if (migrate_from_json(mig)) {
            mig.save(settings_path());
            ini = mig;
        }
    }
    return parse_config(ini);
}

AppConfig load_config(const std::filesystem::path& ini_path) {
    IniFile ini;
    ini.load(ini_path);   // 显式路径：不做 json 迁移
    return parse_config(ini);
}

bool save_config(const AppConfig& cfg, const std::filesystem::path& ini_path) {
    IniFile ini;
    ini.load(ini_path);   // 保留常用扩展名等其他节

    ini.set_int(L"Search", L"threshold_mb", static_cast<int>(cfg.threshold_mb));
    ini.set(L"Search", L"last_root", cfg.last_root);
    // B2：清掉旧版本留在 ini 里的关键词（老用户磁盘上现在就有这么一条）。
    // 每次保存都清一遍，用户无需手工编辑。
    ini.erase(L"Search", L"last_pattern");
    ini.set_bool(L"Search", L"recursive", cfg.recursive);
    ini.set_bool(L"Search", L"use_regex", cfg.use_regex);
    ini.set_bool(L"Search", L"ignore_case", cfg.ignore_case);
    ini.set_bool(L"Search", L"whole_word", cfg.whole_word);
    ini.set_bool(L"Search", L"force_realtime", cfg.force_realtime);
    ini.set_bool(L"Search", L"auto_update_index", cfg.auto_update_index);

    ini.set_int(L"Window", L"x", cfg.window_x);
    ini.set_int(L"Window", L"y", cfg.window_y);
    ini.set_int(L"Window", L"width", cfg.window_w);
    ini.set_int(L"Window", L"height", cfg.window_h);
    ini.set_bool(L"Window", L"maximized", cfg.window_maximized);
    ini.set_int(L"Window", L"left_col_w", cfg.left_col_w);
    ini.set_int(L"Window", L"right_col_w", cfg.right_col_w);
    ini.set_int(L"Window", L"result_col0_w", cfg.result_col0_w);
    ini.set_int(L"Window", L"result_col1_w", cfg.result_col1_w);
    ini.set_int(L"Window", L"result_col2_w", cfg.result_col2_w);
    ini.set_bool(L"Window", L"opt_collapsed", cfg.options_collapsed);
    ini.set_bool(L"Window", L"preview_word_wrap", cfg.preview_word_wrap);
    ini.set(L"Window", L"ui_lang", cfg.ui_lang);
    ini.set_int(L"Search", L"max_results", static_cast<int>(cfg.max_results));
    ini.set_int(L"Search", L"search_timeout_sec", cfg.search_timeout_sec);
    ini.set_int(L"Search", L"watchdog_sec", cfg.watchdog_sec);
    ini.set_int(L"Search", L"history_max", cfg.history_max);
    ini.set_int(L"Preview", L"context_lines", cfg.preview_context_lines);
    ini.set_int(L"Preview", L"max_highlights", cfg.preview_max_highlights);
    ini.set_int(L"Preview", L"large_file_mb", cfg.preview_large_file_mb);
    ini.set_int(L"Preview", L"large_file_ctx", cfg.preview_large_file_ctx);
    ini.set_bool(L"Window", L"dark", cfg.dark);

    ini.set_bool(L"Privacy", L"log_enabled", cfg.log_enabled);
    ini.set_bool(L"Privacy", L"log_redact_paths", cfg.log_redact_paths);

    return ini.save(ini_path);
}

bool save_config(const AppConfig& cfg) {
    return save_config(cfg, settings_path());
}
