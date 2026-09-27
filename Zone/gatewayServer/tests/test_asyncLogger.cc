#include "log/asyncLogger.h"
#include "log/logger.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// AsyncLogger：**不调用 stop() 也必须及时落盘**
//
// 这是线上真的踩过的坑（也是网关端到端测试偶发"看不到日志"的根因）：
//   后端线程只在"缓冲区写满"时被唤醒 -> 低日志量时日志长时间滞留内存 ->
//   进程被 SIGTERM 杀死（没有 stop()/flush 的机会）-> 刚发生的日志永远看不到。
// 断言方式：写一行 -> **不等 stop** 轮询文件 -> 必须在 1 秒内看到它。
// ---------------------------------------------------------------------------
namespace
{
const std::string kDir = "/tmp/clearmoon_async_logger_test";

bool FileContains(const std::string& path, const std::string& needle)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return all.find(needle) != std::string::npos;
}
}  // namespace

TEST(AsyncLoggerFlushesWithoutStop)
{
    ::system(("rm -rf " + kDir).c_str());

    AsyncLogger logger("flush_test", 64 * 1024 * 1024, kDir);
    logger.start();
    Logger::set_AsyncLogger(&logger);
    Logger::set_GlobalLevel(LogLevel::DEBUG);

    const std::string marker = "unique-marker-9f3a1c-should-be-on-disk";
    LOG_INFO << marker;

    // 只给 1 秒：足够覆盖"唤醒 -> swap -> write"的正常路径，
    // 但只要实现退化成"写满才 flush"，这一行就一定看不到（测试即红）
    bool found = false;
    for (int i = 0; i < 100 && !found; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // 文件名形如 <dir>/<yyyy-mm-dd>/flush_test.log
        const std::string day = [] {
            char buf[32];
            std::time_t t = std::time(nullptr);
            std::tm tmBuf{};
            ::localtime_r(&t, &tmBuf);
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmBuf);
            return std::string(buf);
        }();
        found = FileContains(kDir + "/" + day + "/flush_test.log", marker);
    }
    CHECK(found);

    Logger::set_AsyncLogger(nullptr);
    logger.stop();
    ::system(("rm -rf " + kDir).c_str());
}
