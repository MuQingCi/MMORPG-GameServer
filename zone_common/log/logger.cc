#include "logger.h"
#include "asyncLogger.h"
#include <cstddef>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <thread>
#include <iostream>


AsyncLogger* Logger::global_AsyncLogger_ = nullptr;
LogLevel Logger::g_logLevel_ = LogLevel::INFO;

Logger::Logger(LogLevel level, std::string file, size_t line, const char* func) 
            : fileName_(file), 
              line_(line), 
              funcName_(func), 
              enabled_( level >= g_logLevel_),
              logLevel_(level)
{
    if(enabled_)
    {
        auto now = time(nullptr);
        // localtime 返回静态缓冲，多线程同时格式化互相踩数据 -> 必须用 localtime_r
        struct tm tmBuf;
        struct tm* tm = ::localtime_r(&now, &tmBuf);
        std::string today;
        if (tm != nullptr)
        {
            std::ostringstream oss;
            oss << std::put_time(tm, "%Y-%m-%d %H:%M:%S");
            today = oss.str();
        }

        // 旧实现直接 substr(pos)：路径里没有 '/' 时 pos == npos，
        // substr(npos) 会抛 std::out_of_range，把一次日志变成一次崩溃
        const size_t pos = fileName_.rfind('/');
        std::string fName = (pos == std::string::npos) ? fileName_ : fileName_.substr(pos + 1);

        stream_ << "[" << today << "]  thread_id= "
                << std::this_thread::get_id() << "  "
                << LogLevelToString(level) << " "
                << fName << ": "
                << line_ << " ("<< funcName_ << ") | ";
    }
}

Logger::~Logger()
{
    if(!enabled_) return;

    stream_<< "\n";
    const std::string& msg = stream_.str();

    if(global_AsyncLogger_)
        global_AsyncLogger_->append(msg.c_str(), msg.size());
    else
        std::cerr<< msg;
}