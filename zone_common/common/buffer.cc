#include "buffer.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

const char Buffer::kCRLF[] = "\r\n";

// =========== 读取操作 =========== //
int Buffer::read(char* dst, size_t len)
{
    if(len > readableBytes()) 
        len = readableBytes();
    std::copy(peek(), peek() + len, dst);
    retrieve(len);

    return len; 
}

std::string Buffer::readAsString(size_t len)
{
    if(len > readableBytes())
        len = readableBytes();
    std::string result(peek(), peek() + len);
    retrieve(len);
    return result;
}

uint64_t Buffer::readUint64()
{
    static_assert(sizeof(uint64_t) == 8, "int64_t must be 8 Bytes");
    assert(readableBytes() >= sizeof(uint64_t));

    uint64_t netVal;
    memcpy(&netVal, peek(), sizeof(netVal));
    retrieve(sizeof(netVal));

    return netToHost64(netVal);  //大端字节序转主机字节序
}

uint32_t Buffer::readUint32()
{
    static_assert(sizeof(uint32_t) == 4, "int32_t must be 4 Bytes");
    assert(readableBytes() >= sizeof(uint32_t));

    uint32_t netVal;
    memcpy(&netVal, peek(), sizeof(netVal));
    retrieve(sizeof(netVal));

    return netToHost32(netVal);  //大端字节序转主机字节序
}

uint16_t Buffer::readUint16()
{
    static_assert(sizeof(uint16_t) == 2, "int16_t must be 2 Bytes");
    assert(readableBytes() >= sizeof(uint16_t));

    uint16_t netVal;
    memcpy(&netVal, peek(), sizeof(netVal));
    retrieve(sizeof(netVal));

    return netToHost16(netVal);  //大端字节序转主机字节序
}

uint8_t Buffer::readUint8()
{
    assert(readableBytes() >= sizeof(uint8_t));

    uint8_t val = *peek();
    
    retrieve(sizeof(val));

    return val;  // 单字节无需字节序转换
}

const char* Buffer::findCRLF() const
{
    const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
    return (crlf == beginWrite() ? nullptr : crlf);
}

void Buffer::retrieve(size_t len)
{
    assert(len <= readableBytes());
    if(len < readableBytes())
        readIndex_ += len;
    else
        retrieveAll();
}


// =========== 写入操作 =========== //

int Buffer::write(const char* src, size_t len)
{
    ensureWriteable(len);
    std::copy(src, src + len, beginWrite());
    hasWritten(len);
    return static_cast<int>(len);
}


void Buffer::append(const char* src, size_t len)
{
    ensureWriteable(len);
    std::copy(src, src+len, beginWrite());
    hasWritten(len);
}


void Buffer::prepend(const void* data, size_t len)
{
    ensurePrepend(len);
    const char* src = static_cast<const char*>(data);
    readIndex_ -= len;
    std::copy(src, src + len, begin() + readIndex_);
}

void Buffer::ensurePrepend(size_t len)
{
    if (prependBytes() >= len)
        return;

    const size_t readable = readableBytes();
    const size_t keep = len > kCheapPrepend ? len : kCheapPrepend;

    std::vector<char> moved(keep + readable);
    if (readable > 0)
        std::copy(begin() + readIndex_, begin() + writeIndex_, moved.begin() + keep);

    buff_.swap(moved);
    readIndex_ = keep;
    writeIndex_ = keep + readable;
}

// =========== 与文件描述符相关的 =========== //
ssize_t Buffer::readFd(int fd, int* savedErrno)
{
    //65536 = 64 x 1024字节 即64KB 
    char extraData[65536];
    struct iovec vec[2];
    const size_t writeble = writeableBytes();

    vec[0].iov_base = begin() + writeIndex_;
    vec[0].iov_len = writeble;

    vec[1].iov_base = extraData;
    vec[1].iov_len = sizeof extraData;
    
    ssize_t n = ::readv(fd, vec, 2);

    if(n < 0)
        *savedErrno = errno;
    else if(static_cast<size_t>(n) < writeble)
        writeIndex_ += static_cast<size_t>(n);
    else
    {
        writeIndex_ = buff_.size();
        append(extraData, n - writeble);
    }
    return n;
}



ssize_t Buffer::WriteFd(int fd, int* savedErrno)
{
    // MSG_NOSIGNAL：对端已关闭时写不触发 SIGPIPE，避免整个进程被信号杀死
    ssize_t n = ::send(fd, peek(), readableBytes(), MSG_NOSIGNAL);
    if(n < 0)
        *savedErrno = errno;
    else
        retrieve(n);
    return n;
}


// =========== 私有成员 =========== //
void Buffer::ensureWriteable(size_t len)
{
    if(len > writeableBytes())
    {
        // 如果可写空间加上前面预留空间(readIndex - kcheapPrepend) 小于len的话就扩容
        if(writeableBytes() + prependBytes() < len + kCheapPrepend)
        {
            buff_.resize(writeIndex_ + len);
        }
        else 
        {
            size_t readalbeSize = readableBytes();
            std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + kCheapPrepend);
            readIndex_ = kCheapPrepend;
            writeIndex_ = readIndex_ + readalbeSize;
        }
    }
    assert(writeableBytes() >= len);
}

