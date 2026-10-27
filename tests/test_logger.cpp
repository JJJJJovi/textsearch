// tests/test_logger.cpp — B2 日志隐私测试
// 覆盖：路径脱敏（盘符/UNC/家目录定向抹除）、开关（总开关/脱敏开关）、
//       落盘消息里的路径被自动折叠、配置里不留关键词。
// 全部在临时目录进行，不触碰用户真实的 %LOCALAPPDATA%。
#include "simpletest.h"
#include "core/logger.h"
#include "core/config.h"
#include "core/util.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::filesystem::path make_tmp(const std::wstring& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// 读回日志文件全文（UTF-8 → wstring）
std::wstring read_log(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return L"";
    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    return utf8_to_wstring(bytes);
}

// 复位 logger 到出厂状态，避免用例间互相污染
void reset_logger() {
    logger::set_enabled(true);
    logger::set_redact_paths(true);
    logger::set_min_level(logger::Level::Info);
}

} // namespace

// ============================================================
// 用例 1：盘符绝对路径 → 只保留末两级
// ============================================================

TEST_CASE(logger_redact_path_keeps_tail_two) {
    reset_logger();
    std::wstring r = logger::redact_path(
        LR"(C:\Work\AcmeCorp\ProjectX\src\main.cpp)");
    REQUIRE_EQ(r, std::wstring(LR"(...\src\main.cpp)"));
}

// ============================================================
// 用例 2：家目录 C:\Users\<用户名>\... 必须连用户名一起抹掉
// （只留末两级会把 Documents\xxx 留下，但用户名才是重点；
//  这条断言刻意用 Users 之外的目录名，验证定向规则没被"末两级"覆盖掉）
// ============================================================

TEST_CASE(logger_redact_path_strips_home_user) {
    reset_logger();
    std::wstring r = logger::redact_path(
        LR"(C:\Users\alice\Documents\notes.txt)");
    // 段：C: / Users / alice / Documents / notes.txt
    // 末两级 = Documents\notes.txt；用户名 alice 已被越过
    REQUIRE_EQ(r, std::wstring(LR"(...\Documents\notes.txt)"));
    REQUIRE(r.find(L"alice") == std::wstring::npos);

    // 更浅一层：C:\Users\alice\a.txt（末两级 = Users\alice）也必须抹掉 alice
    std::wstring r2 = logger::redact_path(LR"(C:\Users\alice\a.txt)");
    REQUIRE(r2.find(L"alice") == std::wstring::npos);
}

// ============================================================
// 用例 3：UNC 路径
// ============================================================

TEST_CASE(logger_redact_path_unc) {
    reset_logger();
    std::wstring r = logger::redact_path(
        LR"(\\fileserver\share\dept\2026\report.md)");
    REQUIRE_EQ(r, std::wstring(LR"(\\...\2026\report.md)"));
    REQUIRE(r.find(L"fileserver") == std::wstring::npos);
}

// ============================================================
// 用例 4：短路径（≤2 段）原样返回——本来也没什么可隐藏的
// ============================================================

TEST_CASE(logger_redact_path_short_unchanged) {
    reset_logger();
    REQUIRE_EQ(logger::redact_path(LR"(C:\a.txt)"),
               std::wstring(LR"(C:\a.txt)"));
    REQUIRE_EQ(logger::redact_path(LR"(C:\tmp\a.txt)"),
               std::wstring(LR"(C:\tmp\a.txt)"));
    REQUIRE_EQ(logger::redact_path(LR"(D:\Games\demo)"),
               std::wstring(LR"(D:\Games\demo)"));
}

// ============================================================
// 用例 5：脱敏开关关掉 → 原样返回（排障时需要完整路径）
// ============================================================

TEST_CASE(logger_redact_path_disabled) {
    reset_logger();
    const std::wstring full = LR"(C:\Users\alice\Documents\notes.txt)";
    logger::set_redact_paths(false);
    REQUIRE_EQ(logger::redact_path(full), full);
    reset_logger();
}

// ============================================================
// 用例 6（核心）：落盘消息里的路径被自动折叠
// 这是 B2 真正生效的保护——调用方不需要记得手动套 redact_path，
// 以后新增的日志调用也自动受保护。
// ============================================================

TEST_CASE(logger_log_scrubs_embedded_paths) {
    reset_logger();
    auto dir = make_tmp(L"textsearch_log_scrub");
    auto logf = dir / L"t.log";
    logger::set_log_path(logf);

    logger::info(L"开始搜索: 路径=C:\\Users\\alice\\Documents\\secret");
    logger::warn(L"扫描文件失败: C:\\Users\\alice\\Documents\\secret\\notes.txt 编码未知");

    std::wstring body = read_log(logf);
    REQUIRE(!body.empty());
    // 敏感片段（用户名）不得出现
    REQUIRE(body.find(L"alice") == std::wstring::npos);
    // 定位信息仍保留：出问题的那个文件名还在，能对照日志排查
    REQUIRE(body.find(L"secret\\notes.txt") != std::wstring::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================
// 用例 7：总开关关掉 → 一个字节都不落盘
// ============================================================

TEST_CASE(logger_disabled_writes_nothing) {
    reset_logger();
    auto dir = make_tmp(L"textsearch_log_off");
    auto logf = dir / L"t.log";
    logger::set_log_path(logf);

    logger::set_enabled(false);
    logger::info(L"这条不该落盘 C:\\Users\\alice\\x.txt");
    REQUIRE(!std::filesystem::exists(logf));

    // 打开后立刻恢复可写
    logger::set_enabled(true);
    logger::info(L"ok");
    REQUIRE(std::filesystem::exists(logf));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================
// 用例 8：中英标点/空格都能正确切出路径边界
// ============================================================

TEST_CASE(logger_scrub_boundaries) {
    reset_logger();
    auto dir = make_tmp(L"textsearch_log_bound");
    auto logf = dir / L"t.log";
    logger::set_log_path(logf);

    logger::info(L"查看：C:\\Users\\alice\\a\\b.txt，行 12");   // 中文冒号 + 全角逗号
    logger::info(L"[path] C:\\Users\\alice\\a\\b.txt [end]");  // 方括号
    logger::info(L"x=C:\\Users\\alice\\a\\b.txt;y=1");         // 等号 + 分号

    std::wstring body = read_log(logf);
    REQUIRE(body.find(L"alice") == std::wstring::npos);
    // 三处都应折叠出 a\b.txt
    REQUIRE(body.find(L"a\\b.txt") != std::wstring::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================
// 用例 9：配置里不再留关键词——旧版本写在 ini 里的 last_pattern
// 会在下一次保存时被主动清除（老用户磁盘上现在就有这么一条）
// ============================================================

TEST_CASE(config_purges_stale_last_pattern) {
    auto dir = make_tmp(L"textsearch_cfg_privacy");
    auto inif = dir / L"settings.ini";
    {
        std::ofstream ofs(inif, std::ios::binary | std::ios::trunc);
        ofs << "[Search]\r\n"
            << "last_root = D:\\data\r\n"
            << "last_pattern = MyP@ssw0rd-API-KEY\r\n"
            << "recursive = 1\r\n";
    }

    AppConfig cfg = load_config(inif);
    REQUIRE_EQ(cfg.last_root, std::wstring(L"D:\\data"));

    REQUIRE(save_config(cfg, inif));

    std::ifstream ifs(inif, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    REQUIRE(raw.find("MyP@ssw0rd-API-KEY") == std::string::npos);
    REQUIRE(raw.find("last_pattern") == std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================
// 用例 10：日志隐私开关的持久化往返（默认全开保护）
// ============================================================

TEST_CASE(config_log_privacy_roundtrip) {
    auto dir = make_tmp(L"textsearch_cfg_logrt");
    auto inif = dir / L"settings.ini";

    AppConfig cfg;
    REQUIRE(cfg.log_enabled);          // 默认开
    REQUIRE(cfg.log_redact_paths);     // 默认开（安全默认）
    cfg.log_enabled = false;
    cfg.log_redact_paths = false;
    REQUIRE(save_config(cfg, inif));

    AppConfig back = load_config(inif);
    REQUIRE(!back.log_enabled);
    REQUIRE(!back.log_redact_paths);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// O3/O5：结果列表列宽与选项条折叠状态必须能存 ini 再读回（GUI 侧据此记忆布局）
TEST_CASE(config_layout_roundtrip) {
    auto dir = make_tmp(L"textsearch_cfg_layout");
    auto inif = dir / L"settings.ini";

    AppConfig cfg;
    REQUIRE(cfg.result_col0_w == 0);   // 默认 0 → GUI 用缺省
    REQUIRE(!cfg.options_collapsed);   // 默认展开
    cfg.result_col0_w = 480;
    cfg.result_col1_w = 90;
    cfg.result_col2_w = 620;
    cfg.options_collapsed = true;
    REQUIRE(save_config(cfg, inif));

    AppConfig back = load_config(inif);
    REQUIRE(back.result_col0_w == 480);
    REQUIRE(back.result_col1_w == 90);
    REQUIRE(back.result_col2_w == 620);
    REQUIRE(back.options_collapsed);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
