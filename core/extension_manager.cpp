// core/extension_manager.cpp — 常用扩展名管理实现
// （术语约定见 core/extension_manager.h 头注释：UI/配置称「常用扩展名」，
//   代码内部标识符沿用 whitelist，二者为同一份数据）
#include "core/extension_manager.h"
#include "core/config.h"
#include "core/encoding.h"
#include "core/filesystem.h"
#include "core/inifile.h"
#include "core/json.h"
#include "core/text_detect.h"
#include "core/util.h"

#include <algorithm>
#include <fstream>
#include <sstream>

// 线程局部：最后一次 add_extension 的失败原因
thread_local std::wstring g_last_error;

// 配置文件中常用扩展名列表所在的 [节] 与 key。
// UI 上称「常用扩展名」；此处 key 用 common（对外可见的配置项，避免 whitelist 造成误解），
// 读取时仍兼容旧 key `whitelist`。代码内部函数名沿用 whitelist（英文无歧义）。
static const wchar_t* const kCommonSection = L"Extensions";
static const wchar_t* const kCommonKey = L"common";

// 内置预置文本类扩展名列表
std::set<std::wstring> default_whitelist() {
    return {
        L".py",   L".c",   L".cpp",  L".h",   L".hpp",
        L".cc",   L".bas", L".ini",  L".yaml", L".yml",
        L".json", L".toml", L".txt", L".md",  L".xml",
        L".html", L".css", L".js",   L".ts",  L".log",
        L".conf", L".cfg",
    };
}

std::filesystem::path whitelist_path() {
    // 常用扩展名（UI 用语）现存放于统一配置文件 textsearch.ini 的 [Extensions] common
    // （可在「收集扩展名 → 编辑常用扩展名…」中修改）。保留本函数仅为兼容既有调用。
    return settings_path();
}

// 将扩展名规范化为小写 + 前导点
static std::wstring normalize_ext(const std::wstring& ext) {
    std::wstring result = ext;
    // 确保前导点
    if (!result.empty() && result[0] != L'.') {
        result = L"." + result;
    }
    return to_lower_wstring(result);
}

// ---- ExtensionManager 成员 ----

// 旧版常用扩展名文件路径（用于一次性迁移）
static std::filesystem::path legacy_whitelist_path() {
    return get_app_data_dir() / L"whitelist.json";
}

// 从旧版 whitelist.json 迁移常用扩展名到 ini（一次性）。
// 迁移成功后写回 ini，旧 json 文件保留不动。
static std::vector<std::wstring> migrate_legacy_whitelist(IniFile& ini) {
    std::vector<std::wstring> out;
    std::ifstream ifs(legacy_whitelist_path(), std::ios::binary);
    if (!ifs) return out;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    try {
        json::Value v = json::parse(content);
        const json::Value* exts = v.find("extensions");
        if (!exts || !exts->is_array()) return out;
        for (const auto& item : exts->as_array()) {
            if (item.is_string()) {
                out.push_back(utf8_to_wstring(item.as_string()));
            }
        }
        if (!out.empty()) {
            ini.set_list(L"Extensions", kCommonKey, out);
            ini.save(settings_path());
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

std::set<std::wstring> ExtensionManager::load_whitelist() {
    IniFile ini;
    bool loaded = ini.load(settings_path());

    // [Extensions] common 为常用扩展名列表。
    // 若为空，依次回退：旧 key `whitelist`（本工具极早期版本写入）→ 旧 whitelist.json。
    std::vector<std::wstring> items = loaded ? ini.get_list(L"Extensions", kCommonKey)
                                             : std::vector<std::wstring>{};
    if (items.empty() && loaded) {
        items = ini.get_list(L"Extensions", L"whitelist");
    }
    if (items.empty()) {
        items = migrate_legacy_whitelist(ini);
    }

    std::set<std::wstring> result;
    for (const auto& raw : items) {
        std::wstring e = normalize_ext(raw);
        // 空扩展名（目录里"无扩展名"的占位）不入常用扩展名
        if (!e.empty() && e != L".") result.insert(e);
    }
    if (result.empty()) return default_whitelist();
    return result;
}

bool ExtensionManager::save_whitelist(const std::set<std::wstring>& set) {
    // 写入统一配置文件的 [Extensions] common
    IniFile ini;
    ini.load(settings_path());   // 保留 [Search] / [Window] 等其它节
    ini.set_list(L"Extensions", kCommonKey,
                 std::vector<std::wstring>(set.begin(), set.end()));
    // 清掉可能存在的旧 key，避免两处并存造成歧义
    ini.set(L"Extensions", L"whitelist", L"");
    return ini.save(settings_path());
}

bool ExtensionManager::add_extension(const std::wstring& ext,
                                      const std::filesystem::path& sample_dir,
                                      int sample_n) {
    g_last_error.clear();
    std::wstring norm_ext = normalize_ext(ext);

    // 在 sample_dir 递归找该扩展名文件
    std::set<std::wstring> ext_set = {norm_ext};
    auto files = enumerate_files(sample_dir, ext_set, true);

    if (files.empty()) {
        g_last_error = L"目录中未找到扩展名 " + norm_ext + L" 的文件";
        return false;
    }

    // 取前 sample_n 个做抽样验证
    int count = 0;
    for (const auto& fpath : files) {
        if (count >= sample_n) break;
        ++count;

        // 1. is_textual 检测
        if (!is_textual_file(fpath)) {
            g_last_error = L"抽样文件 " + fpath.wstring() + L" 检出 NUL 字节（二进制）";
            return false;
        }
        // 2. read_text 解码成功（非空）
        std::wstring text = read_text(fpath);
        if (text.empty()) {
            g_last_error = L"抽样文件 " + fpath.wstring() + L" 解码失败";
            return false;
        }
    }

    // 全部通过 → 加入常用扩展名并保存
    auto whitelist = load_whitelist();
    whitelist.insert(norm_ext);
    return save_whitelist(whitelist);
}

void ExtensionManager::remove_extension(const std::wstring& ext) {
    std::wstring norm_ext = normalize_ext(ext);
    auto whitelist = load_whitelist();
    whitelist.erase(norm_ext);
    save_whitelist(whitelist);
}

std::wstring last_add_error() {
    return g_last_error;
}

// ---- resolve_ext_set ----

std::set<std::wstring> resolve_ext_set(
    const std::set<std::wstring>& whitelist,
    const std::set<std::wstring>& dir_checked,
    const std::vector<std::wstring>& blacklist,
    const std::vector<std::wstring>& wildcards) {

    // 1. 最终扩展名集合以"用户勾选"为准。
    //    dir_checked 已包含 GUI 预勾选的常用扩展名项（左栏/收集对话框默认勾选常用扩展名），
    //    因此常用扩展名不再无条件并入——否则用户取消勾选的常用扩展名项仍会被搜到（旧 bug）。
    //    仅当用户未勾选任何项（dir_checked 为空）时回退到常用扩展名全集，保持默认全搜行为。
    std::set<std::wstring> base = dir_checked.empty() ? whitelist : dir_checked;
    std::set<std::wstring> result;
    for (const auto& e : base) {
        result.insert(normalize_ext(e));
    }

    // 2. 减去黑名单（精确匹配 + 通配匹配）
    for (const auto& bl : blacklist) {
        // 检查是否为通配模式（含 * 或 ?）
        bool has_wild = bl.find(L'*') != std::wstring::npos ||
                        bl.find(L'?') != std::wstring::npos;
        if (has_wild) {
            // 通配模式：仅转小写，不添加前导点（通配符自带格式）
            std::wstring bl_lower = to_lower_wstring(bl);
            for (auto it = result.begin(); it != result.end(); ) {
                if (wildcard_match(bl_lower, *it)) {
                    it = result.erase(it);
                } else {
                    ++it;
                }
            }
        } else {
            // 精确匹配：normalize（添加前导点 + 小写）
            result.erase(normalize_ext(bl));
        }
    }

    // 3. 减去通配排除模式（如 *.log* 匹配含 .log 的扩展名）
    // 通配符仅转小写，不添加前导点
    for (const auto& wc : wildcards) {
        std::wstring wc_lower = to_lower_wstring(wc);
        for (auto it = result.begin(); it != result.end(); ) {
            if (wildcard_match(wc_lower, *it)) {
                it = result.erase(it);
            } else {
                ++it;
            }
        }
    }

    return result;
}
