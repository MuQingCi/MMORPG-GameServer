#ifndef CLEARMOON_GATEWAY_TESTS_TEST_UTIL_H
#define CLEARMOON_GATEWAY_TESTS_TEST_UTIL_H

// ============================================================================
// 极简测试框架（与 sceneServer/tests/test_util.h 同风格）
//
// 说明：目前两端各自持有一份，是为了不提前引入"跨服务的公共测试库"——
// 那属于阶段 2（抽取公共库）的范围。两份实现保持一致，将来一并上提到公共库。
//
// 用法：
//   TEST(EventLoopRunsInLoopCallback) { CHECK(...); }
// ============================================================================
#include <functional>
#include <string>
#include <vector>

namespace test
{

struct Case
{
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& Registry();

struct Registrar
{
    Registrar(const char* name, std::function<void()> fn);
};

void ReportFailure(const char* file, int line, const std::string& msg);
int& FailureCount();
int& CheckCount();

}  // namespace test

#define TEST(name)                                                    \
    static void name();                                               \
    static test::Registrar test_reg_##name(#name, name);              \
    static void name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        ::test::CheckCount()++;                                                  \
        if (!(cond))                                                             \
            ::test::ReportFailure(__FILE__, __LINE__, "CHECK failed: " #cond);   \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        ::test::CheckCount()++;                                                     \
        if (!((a) == (b)))                                                          \
            ::test::ReportFailure(__FILE__, __LINE__,                               \
                                  std::string("CHECK_EQ failed: " #a " != " #b));   \
    } while (0)

#endif
