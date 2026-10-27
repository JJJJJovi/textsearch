// tests/test_filesystem.cpp — 文件遍历测试
#include "simpletest.h"
#include "core/filesystem.h"
#include "core/util.h"

#include <filesystem>
#include <fstream>

// 创建测试目录结构
static std::filesystem::path make_test_tree() {
    auto base = std::filesystem::temp_directory_path() / "ts_test_fs";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base / "sub1");
    std::filesystem::create_directories(base / "sub2" / "deep");

    // 顶层文件
    std::ofstream(base / "a.py") << "print('hello')";
    std::ofstream(base / "b.c") << "int main(){}";
    std::ofstream(base / "c.png") << "binary";
    std::ofstream(base / "d.ini") << "[cfg]";
    // 子目录文件
    std::ofstream(base / "sub1" / "e.py") << "x=1";
    std::ofstream(base / "sub1" / "f.txt") << "text";
    std::ofstream(base / "sub2" / "g.c") << "void f(){}";
    std::ofstream(base / "sub2" / "deep" / "h.json") << "{}";

    return base;
}

TEST_CASE(fs_non_recursive) {
    auto base = make_test_tree();
    std::set<std::wstring> exts = {L".py", L".c", L".ini", L".json"};
    auto files = enumerate_files(base, exts, false);

    // 非递归只看顶层：a.py, b.c, d.ini
    REQUIRE_EQ(files.size(), static_cast<size_t>(3));

    std::filesystem::remove_all(base);
}

TEST_CASE(fs_recursive) {
    auto base = make_test_tree();
    std::set<std::wstring> exts = {L".py", L".c", L".ini", L".json"};
    auto files = enumerate_files(base, exts, true);

    // 递归：a.py, b.c, d.ini, e.py, g.c, h.json
    REQUIRE_EQ(files.size(), static_cast<size_t>(6));

    std::filesystem::remove_all(base);
}

TEST_CASE(fs_extension_case_insensitive) {
    auto base = std::filesystem::temp_directory_path() / "ts_test_case";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    std::ofstream(base / "upper.PY") << "x";
    std::ofstream(base / "lower.py") << "y";
    std::ofstream(base / "mixed.Py") << "z";

    std::set<std::wstring> exts = {L".py"};
    auto files = enumerate_files(base, exts, false);
    REQUIRE_EQ(files.size(), static_cast<size_t>(3));

    std::filesystem::remove_all(base);
}

TEST_CASE(fs_empty_ext_set) {
    auto base = make_test_tree();
    std::set<std::wstring> empty_exts;
    auto files = enumerate_files(base, empty_exts, true);
    REQUIRE(files.empty());
    std::filesystem::remove_all(base);
}

TEST_CASE(fs_nonexistent_root) {
    std::set<std::wstring> exts = {L".py"};
    auto files = enumerate_files(L"C:\\nonexistent_path_12345", exts, true);
    REQUIRE(files.empty());
}
