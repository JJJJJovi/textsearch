// tests/test_extension_manager.cpp — 扩展名管理测试
#include "simpletest.h"
#include "core/extension_manager.h"
#include "core/util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

// 创建测试目录
static std::filesystem::path make_ext_test_dir() {
    auto base = std::filesystem::temp_directory_path() / "ts_test_ext";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    // 创建一些文本文件
    std::ofstream(base / "a.py") << "print(1)";
    std::ofstream(base / "b.c") << "int main(){}";
    std::ofstream(base / "c.log") << "log line";
    std::ofstream(base / "d.txt") << "text";
    // 创建一个二进制文件
    {
        std::ofstream ofs(base / "fake.bin", std::ios::binary);
        char nul = 0;
        ofs.write("BIN", 3);
        ofs.write(&nul, 1);
        ofs.write("ARY", 2);
    }
    return base;
}

// 使用临时 LOCALAPPDATA 避免污染真实配置
static void setup_test_appdata() {
    auto test_dir = std::filesystem::temp_directory_path() / "ts_test_appdata";
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    _wputenv_s(L"LOCALAPPDATA", test_dir.wstring().c_str());
}

TEST_CASE(ext_default_whitelist) {
    auto wl = default_whitelist();
    REQUIRE(wl.count(L".py") > 0);
    REQUIRE(wl.count(L".c") > 0);
    REQUIRE(wl.count(L".ini") > 0);
    REQUIRE(wl.count(L".txt") > 0);
}

TEST_CASE(ext_load_whitelist_default) {
    setup_test_appdata();
    ExtensionManager mgr;
    // 无 whitelist.json → 返回默认列表
    auto wl = mgr.load_whitelist();
    REQUIRE(wl.count(L".c") > 0);
}

TEST_CASE(ext_save_and_load_whitelist) {
    setup_test_appdata();
    ExtensionManager mgr;
    std::set<std::wstring> custom = {L".py", L".c", L".ini"};
    REQUIRE(mgr.save_whitelist(custom));

    // 重新加载
    auto loaded = mgr.load_whitelist();
    REQUIRE_EQ(loaded.size(), static_cast<size_t>(3));
    REQUIRE(loaded.count(L".py") > 0);
    REQUIRE(loaded.count(L".c") > 0);
    REQUIRE(loaded.count(L".ini") > 0);
}

TEST_CASE(ext_add_extension_valid) {
    setup_test_appdata();
    auto dir = make_ext_test_dir();
    ExtensionManager mgr;
    // 添加 .log（目录中有 c.log 文本文件）
    bool ok = mgr.add_extension(L".log", dir, 3);
    REQUIRE(ok);
    // 验证常用扩展名含 .log
    auto wl = mgr.load_whitelist();
    REQUIRE(wl.count(L".log") > 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE(ext_add_extension_binary_rejected) {
    setup_test_appdata();
    auto dir = make_ext_test_dir();
    ExtensionManager mgr;
    // 添加 .bin（目录中有 fake.bin 含 NUL）
    bool ok = mgr.add_extension(L".bin", dir, 3);
    REQUIRE(!ok);
    // 常用扩展名不应含 .bin
    auto wl = mgr.load_whitelist();
    REQUIRE(wl.count(L".bin") == 0);
    // 错误消息应含信息
    REQUIRE(!last_add_error().empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE(ext_add_extension_not_found) {
    setup_test_appdata();
    auto dir = make_ext_test_dir();
    ExtensionManager mgr;
    // 目录中无 .xyz 文件
    bool ok = mgr.add_extension(L".xyz", dir, 3);
    REQUIRE(!ok);
    std::filesystem::remove_all(dir);
}

TEST_CASE(ext_remove_extension) {
    setup_test_appdata();
    ExtensionManager mgr;
    std::set<std::wstring> wl = {L".py", L".c", L".log"};
    mgr.save_whitelist(wl);
    mgr.remove_extension(L".log");
    auto loaded = mgr.load_whitelist();
    REQUIRE(loaded.count(L".log") == 0);
    REQUIRE(loaded.count(L".py") > 0);
}

// ---- resolve_ext_set 测试 ----

TEST_CASE(ext_resolve_basic) {
    // 勾选{.log}，常用扩展名{.py} 不再无条件并入 —— 结果只含勾选项
    std::set<std::wstring> wl = {L".py"};
    std::set<std::wstring> checked = {L".log"};
    std::vector<std::wstring> blacklist = {L".tmp"};
    std::vector<std::wstring> wildcards;
    auto result = resolve_ext_set(wl, checked, blacklist, wildcards);
    REQUIRE_EQ(result.size(), static_cast<size_t>(1));
    REQUIRE(result.count(L".log") > 0);
    REQUIRE(result.count(L".py") == 0);   // 常用扩展名项未勾选 → 不搜索
}

TEST_CASE(ext_resolve_blacklist_removes) {
    std::set<std::wstring> wl = {L".py", L".c"};
    std::set<std::wstring> checked;
    std::vector<std::wstring> blacklist = {L".c"};
    std::vector<std::wstring> wildcards;
    auto result = resolve_ext_set(wl, checked, blacklist, wildcards);
    REQUIRE_EQ(result.size(), static_cast<size_t>(1));
    REQUIRE(result.count(L".py") > 0);
    REQUIRE(result.count(L".c") == 0);
}

TEST_CASE(ext_resolve_wildcard_removes) {
    // 常用扩展名{.py, .c, .log, .logfile} − 通配 *.log*
    std::set<std::wstring> wl = {L".py", L".c", L".log", L".logfile"};
    std::set<std::wstring> checked;
    std::vector<std::wstring> blacklist;
    std::vector<std::wstring> wildcards = {L"*.log*"};
    auto result = resolve_ext_set(wl, checked, blacklist, wildcards);
    // .py 和 .c 保留, .log 和 .logfile 被通配排除
    REQUIRE(result.count(L".py") > 0);
    REQUIRE(result.count(L".c") > 0);
    REQUIRE(result.count(L".log") == 0);
    REQUIRE(result.count(L".logfile") == 0);
}

TEST_CASE(ext_resolve_case_insensitive) {
    std::set<std::wstring> wl = {L".PY"};
    std::set<std::wstring> checked = {L".Log"};
    std::vector<std::wstring> blacklist;
    std::vector<std::wstring> wildcards;
    auto result = resolve_ext_set(wl, checked, blacklist, wildcards);
    REQUIRE(result.count(L".log") > 0);  // 勾选项（统一小写）
    REQUIRE(result.count(L".py") == 0);  // 常用扩展名项未勾选 → 不并入
}

// 回归：常用扩展名含 .c/.txt/.py，用户只勾 .c（取消 .txt）
// 修复前：常用扩展名无条件并入 → .txt 仍被搜到（bug）
// 修复后：仅搜 .c
TEST_CASE(ext_resolve_excludes_unchecked_whitelist) {
    std::set<std::wstring> wl = {L".c", L".txt", L".py"};
    std::set<std::wstring> checked = {L".c"};
    std::vector<std::wstring> blacklist;
    std::vector<std::wstring> wildcards;
    auto result = resolve_ext_set(wl, checked, blacklist, wildcards);
    REQUIRE_EQ(result.size(), static_cast<size_t>(1));
    REQUIRE(result.count(L".c") > 0);
    REQUIRE(result.count(L".txt") == 0);  // 用户取消勾选 → 排除
    REQUIRE(result.count(L".py") == 0);
}
