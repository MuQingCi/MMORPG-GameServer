#ifndef CLEARMOON_COMMON_BUFFER_H
#define CLEARMOON_COMMON_BUFFER_H

/**
 * @brief 用作收发数据而创建的基类
 * 
 */
#include "noncopy.h"
#include "utils.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <endian.h>
#include <string>
#include <sys/types.h>
#include <vector>

class Buffer : public noncopyable
{
public:
    Buffer(int InitSize = 1024): writeIndex_(kCheapPrepend), readIndex_(kCheapPrepend)
    {
        buff_.resize(kCheapPrepend);
        buff_.reserve(kCheapPrepend + InitSize);
        assert(writeIndex_ == kCheapPrepend);
        assert(readIndex_ == kCheapPrepend);
    }
    
    ~Buffer() = default;

    // =========== 容量查询 =========== //

    /**
     * @brief 返回可读/可写/预留头部 的空间大小
     * 
     * @return size_t 
     */
    size_t readableBytes() const
    {
        return writeIndex_ - readIndex_;
    }

    size_t writeableBytes() const
    {
        return buff_.size() - writeIndex_;
    }

    size_t prependBytes() const
    {
        return readIndex_;
    }

    /**
     * @brief 返回可读数据的的起始指针
     * 
     * @return const char* 
     */
    const char* peek() const
    {
        return begin() + readIndex_;
    }

    // =========== 读取操作 =========== //
    /**
     * @brief 内部调用copy,把peek() -> peek() + len [)之间的数据复制到dst,返回len
     * 
     * @param dst  ---目的指针(char*)
     * @param len  ---复制数据的长度
     * @return int 
     */
    int read(char* dst, size_t len);

    /**
     * @brief 把peek() -> peek() + len [)之间的数据之间构造为string并返回
     * 
     * @param len 
     * @return std::string 
     */
    std::string readAsString(size_t len);
    std::string readAllAsString()
    {
        return readAsString(readableBytes());
    }

    //读取头部无符号Int
    uint64_t readUint64();
    uint32_t readUint32();
    uint16_t readUint16();
    uint8_t readUint8();

    const char* findCRLF() const;

    /**
     * @brief 将readIndex_ + len, 即跳过读完的数据(如果len == readableBytes()就将writeIndex和readIndex_全部重置)
     * 
     * @param len 
     */
    void retrieve(size_t len);
    void retrieveAll()
    {
        writeIndex_ = kCheapPrepend;
        readIndex_ = kCheapPrepend;
    }
    void retrieveUntil(size_t len)
    {
        assert(readIndex_ <= len);
        assert(len < writeIndex_);
        size_t skip = len - readIndex_;
        retrieve(skip);
    }

    // =========== 写入操作 =========== //
    int write(const char* src, size_t len);
    
    int write(const std::string& src)
    {
        return write(src.data(), src.size());
    }
    ///////
    void append(const char* src, size_t len);
    
    void append(const void* src, size_t len)
    {
        append(static_cast<const char*>(src), len);
    }

    //写入预留头部数据
    void prepend(const void* data, size_t len);
    void prependInt64(uint64_t x)
    {
        uint64_t bigInt = host64ToNet(x);
        prepend(&bigInt, sizeof bigInt);
    }
    void prependInt32(uint32_t x)
    {
        uint32_t bigInt = host32ToNet(x);
        prepend(&bigInt, sizeof bigInt);
    }
    void prependInt16(uint16_t x)
    {
        uint16_t bigInt = host16ToNet(x);
        prepend(&bigInt, sizeof bigInt);
    }

    void prependInt8(uint8_t x)
    {
        prepend(&x, sizeof x);
    }

    ///////
    void hasWritten(size_t len)
    {
        writeIndex_ += len;
    }

    char* beginWrite() { return begin() + writeIndex_; }
    const char* beginWrite() const { return begin() + writeIndex_; }


    // =========== 与文件描述符相关的 =========== //
    ssize_t readFd(int fd, int* savedErrno);
    ssize_t WriteFd(int fd, int* savedErrno);

private:
    /**
    * @brief 获取vector<char>的起始位置
             对于vector而言,返回的char*可直接当原始指针迭代器在std::copy()中使用
    * 
    * @return char*  / const char*
    */
    char* begin()
    {
        return buff_.data();
    }

    const char* begin() const
    {
        return buff_.data();
    }

    void ensureWriteable(size_t len);

    static const size_t kCheapPrepend = 8;
    size_t writeIndex_;
    size_t readIndex_;

    static const char kCRLF[];

    std::vector<char> buff_;
};


#endif