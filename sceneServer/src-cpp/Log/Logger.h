#ifndef CLEARMOON_LOG_LOGGER_H
#define CLEARMOON_LOG_LOGGER_H

#include "common/noncopy.h"
#include <cstddef>
#include <sstream>
#include <string>
#include <thread>


class AsyncLogger;

enum LogLevel
{
    DEBUG,
    INFO,
    WARNING,
    ERRNOR,
    OTHER
};

inline std::string LogLevelToString(LogLevel level)
{
    switch (level) 
    {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARNING: return "WARNING";
    case LogLevel::ERRNOR: return "ERROR";
    default: return "UNKNOW";
    }
}

class LogStream
{
public:
    LogStream& operator<<(bool v){ buffer_ += v ? "true" : "false"; return *this; }

    LogStream& operator<<(short v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(unsigned short v){ buffer_ += std::to_string(v); return *this; }

    LogStream& operator<<(int v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(unsigned int v){ buffer_ += std::to_string(v); return *this; }

    LogStream& operator<<(long v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(unsigned long v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(long long v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(unsigned long long v){ buffer_ += std::to_string(v); return *this; }

    LogStream& operator<<(float v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(double v){ buffer_ += std::to_string(v); return *this; }
    LogStream& operator<<(long double v){ buffer_ += std::to_string(v); return *this; }
    
    LogStream& operator<<(char v){ buffer_ += v; return *this; }
    LogStream& operator<<(const char* v){ buffer_ += v; return *this; }
    LogStream& operator<<(const std::string& v){ buffer_ += v; return *this; }

    LogStream& operator<<(std::thread::id v) 
    {
        std::ostringstream oss;
        oss << v;
        buffer_ += oss.str();
        return *this;
    }
    LogStream& operator<<(void* v)
    {
        std::ostringstream oss;
        oss<<v;
        buffer_ += oss.str();
        return *this;
    }

    const std::string& str() const { return buffer_; };
    void reset() { buffer_.clear(); }
private:
    std::string buffer_;
};

class Logger : noncopyable
{
public:
    Logger(LogLevel level, std::string file, size_t line, const char* func);

    ~Logger();

    LogStream& stream() { return stream_; }

    static void set_AsyncLogger(AsyncLogger* asynclogger) { global_AsyncLogger_ = asynclogger; }
    static AsyncLogger* get_AsyncLogger() { return global_AsyncLogger_; }

    static void set_GlobalLevel(LogLevel level) { g_logLevel_ = level; }
    static LogLevel get_GlobalLevel() { return g_logLevel_; }

private:
    static AsyncLogger* global_AsyncLogger_;
    static LogLevel g_logLevel_;

    std::string fileName_;
    size_t line_;
    const char* funcName_;
    bool enabled_;

    LogLevel logLevel_;
    LogStream stream_;
};





//--------------------------------------------------日志宏定义--------------------------------------------------

#define LOG_DEBUG Logger(LogLevel::DEBUG,    __FILE__, __LINE__, __func__).stream()
#define LOG_INFO Logger(LogLevel::INFO,      __FILE__, __LINE__, __func__).stream()
#define LOG_WARNING Logger(LogLevel::WARNING,__FILE__, __LINE__, __func__).stream()
#define LOG_ERROR Logger(LogLevel::ERRNOR,    __FILE__, __LINE__, __func__).stream()

#endif