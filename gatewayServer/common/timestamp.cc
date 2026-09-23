#include "common/timestamp.h"
#include <bits/types/struct_timeval.h>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/types.h>

const int Timestamp::kMicroSecondPerSecond = 1000 * 1000;

/**
 * @brief 
 * 
 * @return Timestamp 
 */
Timestamp Timestamp::now()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t microsecond = static_cast<int64_t>(ts.tv_sec) * kMicroSecondPerSecond + static_cast<int64_t>(ts.tv_nsec / 1000);

    return Timestamp(microsecond);
}

std::string Timestamp::toFormattedString()
{
    if(!isValid()) return "Invalid";
    int64_t second = microSecondsSinceEpoch_ / kMicroSecondPerSecond;
    int64_t microsecond = microSecondsSinceEpoch_ % kMicroSecondPerSecond;

    struct tm time;
    localtime_r(&second, &time);

    char buf[32];
    
    size_t len = snprintf(buf, sizeof buf, "%4d%02d%02d %02d:%02d:%02d", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);

    snprintf(buf + len, sizeof buf - len, ".%06ld", microsecond);
    
    return std::string(buf);
}

std::string Timestamp::toDateString()
{
    if(!isValid()) return "Invalid";
    int64_t second = microSecondsSinceEpoch_ / kMicroSecondPerSecond;

    struct tm time;
    localtime_r(&second, &time);

    char buf[32];
    
    (void)snprintf(buf, sizeof buf, "%4d%02d%02d", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday);   // 返回值（写入长度）在本函数中不需要

    return std::string(buf);
}
