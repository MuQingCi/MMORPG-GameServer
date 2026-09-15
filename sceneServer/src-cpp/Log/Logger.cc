#include "Logger.h"
#include "AsyncLogger.h"
#include <cstddef>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <thread>
#include <iostream>


AsyncLogger* Logger::global_AsyncLogger_ = nullptr;
LogLevel Logger::g_logLevel_ = LogLevel::INFO;

Logger::Logger(LogLevel level, std::string file, size_t line, const char* func) 
            : logLevel_(level), 
              fileName_(file), 
              line_(line), 
              funcName_(func), 
              enabled_( level >= g_logLevel_)
{
    if(enabled_)
    {
        auto now = time(nullptr);
        auto tm = *localtime(&now);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        std::string today =oss.str();

        size_t pos = fileName_.rfind('/');
        std::string fName = fileName_.substr(pos);

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