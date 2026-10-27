// tests/test_i18n.cpp — 界面多语言回归（tr/trf/回退/语言解析）
#include "simpletest.h"
#include "core/i18n.h"

TEST_CASE(i18n_zh_baseline) {
    i18n::set_language_from(L"zh");
    REQUIRE(std::wstring(i18n::tr("btn.search")) == L"搜索");
    REQUIRE(std::wstring(i18n::tr("sb.index_ok")) == L"索引：已建立");
}

TEST_CASE(i18n_en_switch) {
    i18n::set_language_from(L"en");
    REQUIRE(std::wstring(i18n::tr("btn.search")) == L"Search");
    REQUIRE(std::wstring(i18n::tr("sb.index_ok")) == L"Index: built");
    // 参数化模板
    REQUIRE(i18n::trf("sb.count_files", L"3", L"9") == L"3 / 9 files");
    i18n::set_language_from(L"zh");
    REQUIRE(i18n::trf("sb.count_files", L"3", L"9") == L"3 / 9 文件");
}

TEST_CASE(i18n_missing_fallback) {
    // 无条目 → id 本身（ASCII 透传，开发期可见缺口）
    REQUIRE(std::wstring(i18n::tr("no.such.id")) == L"no.such.id");
}

TEST_CASE(i18n_system_resolution) {
    // system → 依 GetUserDefaultUILanguage 解析为 zh/en 之一（不断言具体值，
    // 只断言解析后 tr 返回非空且非 id——表内条目必然命中）
    i18n::set_language_from(L"system");
    std::wstring v = i18n::tr("btn.search");
    REQUIRE(v == L"搜索" || v == L"Search");
}
