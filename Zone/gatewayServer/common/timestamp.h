#ifndef CLEARMOON_GATEWAY_COMMON_TIMESTAMP_H
#define CLEARMOON_GATEWAY_COMMON_TIMESTAMP_H

#include <cstdint>
#include <string>
#include <sys/types.h>


/**
        时间类Timestamp
持有：私有成员int64_t microSecondSinceEpoch_  ---微秒
公开接口:  
    静态接口: now()                          ----获取当前时间
             invalid()                      ----创建一个不合法的时间类Timestamp
    普通接口:
            setxxx/getxxx()                 ----设置/获取成员变量
            toDateString/toFormattedString()----将该对象转换成对应的字符串
            isvalid()                       ----判断该对象是否合法
            differMicroSecond()             ----获取目标对象与当前对象的时间差的绝对值
*/


class Timestamp
{
public:
    static const int kMicroSecondPerSecond;

    explicit Timestamp(int64_t mircroSeconds) : microSecondsSinceEpoch_(mircroSeconds)
    {
    }

    Timestamp() : microSecondsSinceEpoch_(0)
    {
    }
    ~Timestamp() = default;

    static Timestamp now();
    static Timestamp invalid() { return Timestamp(-1); } ;

    int64_t getMicroSecond() const { return  microSecondsSinceEpoch_; }
    void setMicroSecond(int64_t MicroSecond) { microSecondsSinceEpoch_ = MicroSecond; }


    bool isValid() const { return microSecondsSinceEpoch_ > 0;}

    //To string 
    std::string toFormattedString();   //YYYYMMDD hh:mm:ss.xxxx
    std::string toDateString();        //YYYYMMDD

    //Other operation interfaces
    int64_t differMicroSecond(const Timestamp& rhs) const
    {
        return std::abs(rhs.getMicroSecond() - microSecondsSinceEpoch_);
    }

    bool operator<(const Timestamp& rhs) const
    {
        return microSecondsSinceEpoch_ < rhs.microSecondsSinceEpoch_;
    }
    
private:
    int64_t microSecondsSinceEpoch_;
};

#endif