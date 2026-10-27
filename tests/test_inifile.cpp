// tests/test_inifile.cpp — INI 配置文件读写测试
// 覆盖：键值往返（字符串/整数/布尔/列表）、注释与空白处理、UTF-8 BOM、
//       缺失键返回默认值、key 大小写不敏感、非法整数回退默认值。
// 全部在临时目录进行，不触碰用户真实配置。
#include "simpletest.h"
#include "core/inifile.h"
#include "core/util.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// 建临时目录
std::filesystem::path make_ini_dir(const std::wstring& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// 写 UTF-8 带 BOM 的原始文本（用于测试 BOM 与注释解析）
bool write_raw(const std::filesystem::path& p, const std::wstring& text) {
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    const char bom[3] = {'\xEF', '\xBB', '\xBF'};
    ofs.write(bom, 3);
    std::string u8 = wstring_to_utf8(text);
    ofs.write(u8.data(), static_cast<std::streamsize>(u8.size()));
    return ofs.good();
}

// 读回原始文本（跳过 BOM），用于校验生成格式
std::wstring read_raw(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return L"";
    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    size_t off = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        off = 3;
    }
    return utf8_to_wstring(bytes.substr(off));
}

} // namespace

// ---- 1. 写入后读回：字符串 / 整数 / 布尔 ----
TEST_CASE(inifile_roundtrip_basic) {
    auto dir = make_ini_dir(L"textsearch_ini_basic");
    auto path = dir / L"test.ini";

    IniFile w;
    w.set(L"Search", L"last_root", L"D:\\some\\path");
    w.set_int(L"Search", L"threshold_mb", 80);
    w.set_bool(L"Search", L"recursive", true);
    w.set_bool(L"Search", L"use_regex", false);
    REQUIRE(w.save(path));

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get(L"Search", L"last_root") == L"D:\\some\\path");
    REQUIRE(r.get_int(L"Search", L"threshold_mb", 50) == 80);
    REQUIRE(r.get_bool(L"Search", L"recursive", false) == true);
    REQUIRE(r.get_bool(L"Search", L"use_regex", true) == false);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 2. 逗号分隔列表往返（常用扩展名用）----
TEST_CASE(inifile_list_roundtrip) {
    auto dir = make_ini_dir(L"textsearch_ini_list");
    auto path = dir / L"test.ini";

    std::vector<std::wstring> wl = {L".py", L".c", L".cpp", L".txt", L".md"};
    IniFile w;
    w.set_list(L"Extensions", L"whitelist", wl);
    REQUIRE(w.save(path));

    IniFile r;
    REQUIRE(r.load(path));
    auto got = r.get_list(L"Extensions", L"whitelist");
    REQUIRE_EQ(got.size(), static_cast<size_t>(5));
    REQUIRE(got[0] == L".py");
    REQUIRE(got[4] == L".md");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 3. 注释 / 空行 / 行尾注释 应被忽略 ----
TEST_CASE(inifile_comments_ignored) {
    auto dir = make_ini_dir(L"textsearch_ini_comment");
    auto path = dir / L"test.ini";

    std::wstring content =
        L"; 这是注释行\n"
        L"# 这也是注释\n"
        L"\n"
        L"[Search]   \n"
        L"threshold_mb = 120   ; 行尾注释\n"
        L"last_root = E:\\data\n";
    REQUIRE(write_raw(path, content));

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get_int(L"Search", L"threshold_mb", 50) == 120);
    REQUIRE(r.get(L"Search", L"last_root") == L"E:\\data");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 4. UTF-8 BOM：带 BOM 的文件能正确读取中文 ----
TEST_CASE(inifile_utf8_bom) {
    auto dir = make_ini_dir(L"textsearch_ini_bom");
    auto path = dir / L"test.ini";

    IniFile w;
    w.set(L"Section", L"name", L"中文值");
    REQUIRE(w.save(path));

    // 保存时应带 BOM
    std::wstring raw = read_raw(path);
    REQUIRE(raw.find(L"中文值") != std::wstring::npos);

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get(L"Section", L"name") == L"中文值");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 5. 缺失键 / 缺失节 → 返回默认值；非法整数回退默认值 ----
TEST_CASE(inifile_defaults) {
    auto dir = make_ini_dir(L"textsearch_ini_default");
    auto path = dir / L"test.ini";

    REQUIRE(write_raw(path, L"[Search]\nthreshold_mb = not_a_number\n"));

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get_int(L"Search", L"threshold_mb", 50) == 50);   // 非法 → 默认
    REQUIRE(r.get(L"Search", L"missing") == L"");               // 缺键 → 默认
    REQUIRE(r.get(L"Missing", L"anything", L"fallback") == L"fallback");  // 缺节 → 默认
    REQUIRE(r.get_int(L"Missing", L"x", 7) == 7);
    REQUIRE(r.get_bool(L"Missing", L"b", true) == true);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 6. key 大小写不敏感（用户手写 ini 可能大小写混用）----
TEST_CASE(inifile_key_case_insensitive) {
    auto dir = make_ini_dir(L"textsearch_ini_case");
    auto path = dir / L"test.ini";

    REQUIRE(write_raw(path, L"[Search]\nThreshold_MB = 90\n"));

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get_int(L"Search", L"threshold_mb", 50) == 90);
    REQUIRE(r.get_int(L"Search", L"THRESHOLD_MB", 50) == 90);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 7. 加载不存在的文件返回 false，且不抛异常 ----
TEST_CASE(inifile_load_missing) {
    auto dir = make_ini_dir(L"textsearch_ini_missing");
    IniFile r;
    REQUIRE(!r.load(dir / L"nope.ini"));
    REQUIRE(r.get(L"Search", L"x", L"def") == L"def");
    REQUIRE(r.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 8. 常用扩展名 key 兼容：优先 common，缺失时回退旧 key whitelist ----
// （UI 称「常用扩展名」，配置 key 为 common；早期版本写入的是 whitelist）
TEST_CASE(inifile_common_key_with_legacy_fallback) {
    auto dir = make_ini_dir(L"textsearch_ini_common_key");
    auto path = dir / L"test.ini";

    // 场景一：只有旧 key → 回退使用
    REQUIRE(write_raw(path, L"[Extensions]\nwhitelist = .py, .c\n"));
    {
        IniFile r;
        REQUIRE(r.load(path));
        REQUIRE(r.get_list(L"Extensions", L"common").empty());          // 新 key 无
        REQUIRE_EQ(r.get_list(L"Extensions", L"whitelist").size(),
                   static_cast<size_t>(2));                             // 旧 key 可用
    }

    // 场景二：两者并存 → 新 key 优先
    REQUIRE(write_raw(path, L"[Extensions]\ncommon = .md\nwhitelist = .py, .c\n"));
    {
        IniFile r;
        REQUIRE(r.load(path));
        REQUIRE(r.get(L"Extensions", L"common") == L".md");
        REQUIRE(r.get_list(L"Extensions", L"common").size() == 1);
    }

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- 9. 保存会保留其它已有节（config 的 save 依赖此行为，不能整份覆盖）----
TEST_CASE(inifile_save_preserves_other_sections) {
    auto dir = make_ini_dir(L"textsearch_ini_preserve");
    auto path = dir / L"test.ini";

    IniFile w1;
    w1.set_list(L"Extensions", L"whitelist", {L".py", L".c"});
    w1.set_int(L"Window", L"width", 1000);
    REQUIRE(w1.save(path));

    // 模拟 config::save：只改 [Search]，其余节必须原样保留
    IniFile w2;
    REQUIRE(w2.load(path));
    w2.set_bool(L"Search", L"recursive", false);
    REQUIRE(w2.save(path));

    IniFile r;
    REQUIRE(r.load(path));
    REQUIRE(r.get_list(L"Extensions", L"whitelist").size() == 2);  // 保留
    REQUIRE(r.get_int(L"Window", L"width", 0) == 1000);            // 保留
    REQUIRE(r.get_bool(L"Search", L"recursive", true) == false);   // 已更新

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
