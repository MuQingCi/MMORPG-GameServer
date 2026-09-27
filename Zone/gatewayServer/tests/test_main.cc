#include "test_util.h"

#include "log/logger.h"

#include <cstdio>
#include <utility>

namespace test
{
namespace
{
int g_failures = 0;
int g_checks = 0;
}  // namespace

std::vector<Case>& Registry()
{
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(const char* name, std::function<void()> fn)
{
    Registry().push_back(Case{name, std::move(fn)});
}

void ReportFailure(const char* file, int line, const std::string& msg)
{
    ++g_failures;
    std::fprintf(stderr, "[FAIL] %s:%d %s\n", file, line, msg.c_str());
}

int& FailureCount()
{
    return g_failures;
}

int& CheckCount()
{
    return g_checks;
}
}  // namespace test

int main()
{
    // 正常路径的 INFO 日志会淹没断言结果，默认只输出 ERROR
    Logger::set_GlobalLevel(LogLevel::ERROR);

    int failedCases = 0;
    for (auto& c : test::Registry())
    {
        const int before = test::FailureCount();
        c.fn();
        const bool ok = (test::FailureCount() == before);
        std::fprintf(stdout, "[%s] %s\n", ok ? " OK " : "FAIL", c.name.c_str());
        if (!ok)
            ++failedCases;
    }

    std::fprintf(stdout, "----------------------------------------\n");
    std::fprintf(stdout, "cases=%zu failed_cases=%d checks=%d failures=%d\n",
                 test::Registry().size(), failedCases, test::CheckCount(),
                 test::FailureCount());
    return failedCases == 0 ? 0 : 1;
}
