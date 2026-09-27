#ifndef CLEARMOON_TESTS_TEST_UTIL_H
#define CLEARMOON_TESTS_TEST_UTIL_H

// ============================================================================
// 极简测试框架（不引入第三方依赖）
//
// 为什么不直接用 Catch2/gtest：
//   本仓库当前没有包管理/离线依赖约束，引入 framework 会连带一个网络下载点；
//   而这里需要的只是"注册用例 + 断言 + 统计失败数"，几十行就够。
//   将来接入 CI 时换成 Catch2 也很容易（断言宏语义一致）。
//
// 用法：
//   TEST(ProtoRoundTrip) {
//       CHECK_EQ(a, b);
//   }
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
