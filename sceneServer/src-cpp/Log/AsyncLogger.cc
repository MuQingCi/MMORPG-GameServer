#include "AsyncLogger.h"
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
    if(!running_) return;
    running_ = false;
    cond_.notify_one();

    if(backThread_.joinable())
        backThread_.join();
    
}

void AsyncLogger::append(const char* data, size_t len)
{
    if(currentBuffer_->writeableBytes() > len)
        currentBuffer_->append(data, len);
    else
    {
        buffers_.push_back(std::move(currentBuffer_));
        if(nextBuffer_)
            currentBuffer_ = std::move(nextBuffer_);
        else
        {
            currentBuffer_.reset(new Buffer);
        }

        currentBuffer_->append(data, len);
        cond_.notify_one();
    };
}

void AsyncLogger::threadFunc()
{
    BufferPtr buffA(new Buffer);
    BufferPtr buffB(new Buffer);
    BufferVector buffersToWrite;

    while(running_)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            cond_.wait(lock, [this] {
                return !running_ || !buffers_.empty() ;}
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
    }
}

void AsyncLogger::write(const char* data, size_t len)
{
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    std::ostringstream dataStream;
    dataStream << std::put_time(&tm, "%Y-%m-%d");

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