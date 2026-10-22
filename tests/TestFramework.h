#pragma once
// Minimal test framework (no external dependency).
//
//   TEST_CASE("name") { CHECK(a == b); CHECK_NEAR(x, y, 1e-6); REQUIRE(ptr != nullptr); }
//
// Run: ./build/tests/sonora_tests [filter-substring]

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sonora::test
{

struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int n = 0;
    return n;
}

struct Registrar
{
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct RequireFailed
{
};

} // namespace sonora::test

#define SONORA_CAT2(a, b) a##b
#define SONORA_CAT(a, b) SONORA_CAT2(a, b)

#define TEST_CASE(name)                                                                                              \
    static void SONORA_CAT(test_fn_, __LINE__)();                                                                    \
    static ::sonora::test::Registrar SONORA_CAT(test_reg_, __LINE__)(name, &SONORA_CAT(test_fn_, __LINE__));         \
    static void SONORA_CAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                                                                  \
    do                                                                                                               \
    {                                                                                                                \
        if (! (expr))                                                                                                \
        {                                                                                                            \
            ++::sonora::test::failureCount();                                                                        \
            std::printf("    FAILED %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #expr);                                \
        }                                                                                                            \
    } while (false)

#define CHECK_NEAR(a, b, eps)                                                                                        \
    do                                                                                                               \
    {                                                                                                                \
        const double _a = (a), _b = (b);                                                                             \
        if (std::abs(_a - _b) > (eps))                                                                               \
        {                                                                                                            \
            ++::sonora::test::failureCount();                                                                        \
            std::printf("    FAILED %s:%d  CHECK_NEAR(%s, %s) -> %.9g vs %.9g\n", __FILE__, __LINE__, #a, #b, _a, _b); \
        }                                                                                                            \
    } while (false)

#define REQUIRE(expr)                                                                                                \
    do                                                                                                               \
    {                                                                                                                \
        if (! (expr))                                                                                                \
        {                                                                                                            \
            ++::sonora::test::failureCount();                                                                        \
            std::printf("    FAILED %s:%d  REQUIRE(%s)\n", __FILE__, __LINE__, #expr);                              \
            throw ::sonora::test::RequireFailed{};                                                                   \
        }                                                                                                            \
    } while (false)
