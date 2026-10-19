#include "log/asyncLogger.h"
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sys/stat.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>


void AsyncLogger::start()
{
    if(running_) return;
    running_ = true;
    backThread_ = std::thread([this]{ threadFunc(); });
}

void AsyncLogger::stop()
{
    {
        // 在与等待操作相同的互斥锁下修改谓词，防止在谓词检查与进入休眠之间丢失通知
        std::lock_guard<std::mutex> lock(mutex_);
        if(!running_) return;
        running_ = false;
    }
    cond_.notify_one();

    if(backThread_.joinable())
        backThread_.join();
    
}

void AsyncLogger::append(const char* data, size_t len)
{
    // 必须加锁：append 会被任意业务线程调用，而 buffers_/currentBuffer_以及 Buffer 本身都不是线程安全的（旧实现无锁 -> 多线程下必然踩数据）
    std::lock_guard<std::mutex> lock(mutex_);

    if(currentBuffer_->writeableBytes() > len)
    {
        currentBuffer_->append(data, len);
        // 等待谓词也检查 currentBuffer_：所有已完成缓冲区为空时，仅发送通知无法使带谓词的等待结束
        cond_.notify_one();
        return;
    }

    buffers_.push_back(std::move(currentBuffer_));
    if(nextBuffer_)
        currentBuffer_ = std::move(nextBuffer_);
    else
        currentBuffer_.reset(new Buffer);

    currentBuffer_->append(data, len);
    cond_.notify_one();
}

void AsyncLogger::threadFunc()
{
    BufferPtr buffA(new Buffer);
    BufferPtr buffB(new Buffer);
    BufferVector buffersToWrite;

    while(true)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            cond_.wait(lock, [this] {
                return !running_ || !buffers_.empty() || currentBuffer_->readableBytes() > 0;}
            );

            if(!buffers_.empty() || currentBuffer_->readableBytes() > 0)
            {
                buffers_.push_back(std::move(currentBuffer_));
                currentBuffer_ = std::move(buffA);
                buffersToWrite.swap(buffers_);
                if(!nextBuffer_)
                    nextBuffer_ = std::move(buffB);
            }
            else if(!running_)
                return;
        }
        
        for(const auto& buf : buffersToWrite)
            write(buf->peek(), buf->readableBytes());
        
        for(const auto& buf : buffersToWrite)
            buf->retrieveAll();

        if(buffA == nullptr)
        {
            buffA = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
        }

        if(buffB == nullptr)
        {
            if(!buffersToWrite.empty())
            {
                buffB = std::move(buffersToWrite.back());
                buffersToWrite.pop_back();
            }
        }
        // 本批缓冲区已全部消费，只保留两个备用缓冲区
        buffersToWrite.clear();
    }
}

void AsyncLogger::write(const char* data, size_t len)
{
    auto now = std::time(nullptr);
    struct tm tmBuf;
    struct tm* tm = ::localtime_r(&now, &tmBuf);
    if (tm == nullptr)
        return;

    std::ostringstream dataStream;
    dataStream << std::put_time(tm, "%Y-%m-%d");

    std::string today = dataStream.str();

    std::string dataDir = logDir_ + "/" + today;
    ::mkdir(dataDir.c_str(), 0755);
    
    if(today != lastData_)
    {
        file_.close();
        sequence_ = 0;
        hasWritenBytes_ = 0;
        lastData_ = today;
    }

    std::string fileName = makeFileName(today);
    if(!file_.is_open() || currentFileName_ != fileName)
    {
        file_.close();
        file_.open(fileName, std::ios::app);
        if(!file_)
        {
            fprintf(stderr, "AsyncLogger: faild to open %s\n", fileName.c_str());
            return;
        }

        currentFileName_ = fileName;
        
        struct stat st;

        if(::stat(fileName.c_str(), &st) == 0)
        {
            hasWritenBytes_ = st.st_size;
        }
        else {
            hasWritenBytes_ = 0;
        }

        if(hasWritenBytes_ + len > rollSize_ && hasWritenBytes_ >0)
        {
            file_.close();
            sequence_++;
            fileName = makeFileName(today);
            file_.open(fileName, std::ios::app);
            if(!file_)
            {
                fprintf(stderr, "AsyncLogger: faild to open %s\n", fileName.c_str());
                return;
            }
            currentFileName_ = fileName;
            hasWritenBytes_ = 0;
        }
    }
    file_.write(data, len);
    if(file_.fail())
    {
        fprintf(stderr, "AsyncLogger: write failed");
    }
    file_.flush();
    hasWritenBytes_ += len;
}

std::string AsyncLogger::makeFileName(std::string& data)
{
    std::ostringstream oss;
    oss << logDir_ << "/" << data << "/" << name_;
    if(sequence_ > 0)
    {
        oss << "." << sequence_;
    }
    oss<< ".log";
    return oss.str();
}