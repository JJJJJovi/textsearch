// tests/simpletest.h — 轻量级测试框架（header-only，不依赖 Catch2）
// 当 vendor/catch2/ 可用后可迁移到 Catch2
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace simpletest {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

// 测试失败异常
struct TestFailure : std::runtime_error {
    explicit TestFailure(const std::string& msg) : std::runtime_error(msg) {}
};

// 运行所有注册的测试，返回失败数
inline int run_all() {
    int passed = 0;
    int failed = 0;
    for (const auto& tc : registry()) {
        try {
            tc.func();
            std::cout << "[PASS] " << tc.name << std::endl;
            ++passed;
        } catch (const TestFailure& e) {
            std::cout << "[FAIL] " << tc.name << " - " << e.what() << std::endl;
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[ERROR] " << tc.name << " - " << e.what() << std::endl;
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
    return failed > 0 ? 1 : 0;
}

} // namespace simpletest

// ---- 宏 ----

// 注册测试用例
#define TEST_CASE(name)                                              \
    static void ts_##name();                                         \
    static bool ts_reg_##name =                                      \
        (::simpletest::registry().push_back({#name, ts_##name}), true); \
    static void ts_##name()

// 断言表达式为真
#define REQUIRE(expr)                                               \
    do {                                                            \
        if (!(expr)) {                                              \
            throw ::simpletest::TestFailure(                        \
                std::string("REQUIRE failed: " #expr " at ") +      \
                __FILE__ + ":" + std::to_string(__LINE__));         \
        }                                                           \
    } while(0)

// 断言相等
#define REQUIRE_EQ(a, b)                                            \
    do {                                                            \
        auto ts_a = (a);                                            \
        auto ts_b = (b);                                            \
        if (!(ts_a == ts_b)) {                                      \
            throw ::simpletest::TestFailure(                        \
                std::string("REQUIRE_EQ failed: " #a " != " #b     \
                            " at ") + __FILE__ + ":" +              \
                std::to_string(__LINE__));                          \
        }                                                           \
    } while(0)
