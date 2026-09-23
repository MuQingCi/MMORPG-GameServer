#ifndef CLEARMOON_LOG_ASYNCLOGGER_H
#define CLEARMOON_LOG_ASYNCLOGGER_H

#include "common/buffer.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>


class AsyncLogger : public noncopyable
{
public:
using BufferPtr = std::unique_ptr<Buffer>;
using BufferVector = std::vector<BufferPtr>;
    AsyncLogger(const std::string& name, size_t rollsize, const std::string& fileDir) : name_(name), rollSize_(rollsize), currentBuffer_(new Buffer), nextBuffer_(new Buffer), running_(false), logDir_(fileDir)
    {
        ::mkdir(logDir_.c_str(), 0755);
    }

    void start();
    void stop();

    void append(const char* data, size_t len);
private:
    void threadFunc();
    void write(const char* data, size_t len);

    std::string makeFileName(std::string& data);

    //互斥锁与条件变量
    std::mutex mutex_;
    std::condition_variable cond_;

    //日志名称与单日志最大行数
    std::string name_;
    size_t rollSize_;

    //缓冲区
    BufferPtr currentBuffer_;
    BufferPtr nextBuffer_;
    BufferVector buffers_;

    //后端线程(用于写入磁盘)
    std::thread backThread_;

    //开始标志
    std::atomic<bool> running_;

    std::ofstream file_;
    std::string lastData_;
    std::string currentFileName_;
    size_t sequence_;
    size_t hasWritenBytes_;

    const std::string logDir_;
};

#endif