#include "base/buffer.h"
#include "test_util.h"

#include <string>

TEST(BufferAppendAndRetrieve)
{
    Buffer b;
    b.append("hello", 5);
    b.append("world", 5);
    CHECK_EQ(b.readableBytes(), (size_t)10);

    std::string s = b.readAsString(5);
    CHECK(s == "hello");
    CHECK_EQ(b.readableBytes(), (size_t)5);

    b.retrieve(2);
    CHECK_EQ(b.readableBytes(), (size_t)3);
    CHECK(std::string(b.peek(), 3) == "rld");

    b.retrieveAll();
    CHECK_EQ(b.readableBytes(), (size_t)0);
}

// 帧编码依赖"向前预留 32 字节"：可读数据被部分消费后仍必须能 prepend
TEST(BufferEnsurePrepend)
{
    Buffer b;
    b.append("0123456789", 10);
    b.retrieve(6);   // readIndex 缩小，头部预留空间不再够 32 字节

    CHECK_EQ(b.readableBytes(), (size_t)4);

    std::string header(32, 'H');
    b.prepend(header.data(), header.size());

    CHECK_EQ(b.readableBytes(), (size_t)36);
    CHECK(std::string(b.peek(), 32) == header);
    CHECK(std::string(b.peek() + 32, 4) == "6789");
}

TEST(BufferPrependOrder)
{
    Buffer b;
    b.append("BODY", 4);

    b.prependInt8(0x31);
    b.prependInt8(0x30);
    b.prependInt16(0x7788);

    // 网络序：先 prepend 的在后
    CHECK_EQ(b.readableBytes(), (size_t)8);
    CHECK_EQ(b.readUint16(), (uint16_t)0x7788);
    CHECK_EQ(b.readUint8(), (uint8_t)0x30);
    CHECK_EQ(b.readUint8(), (uint8_t)0x31);
    CHECK(std::string(b.peek(), 4) == "BODY");
}
