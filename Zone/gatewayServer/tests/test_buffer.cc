#include "base/buffer.h"
#include "test_util.h"

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 收发缓冲：追加/读取/prepend/半包拼接
//   （Buffer 属于公共库 zone_common，由 gateway_core 链接进来；
//    这里从"网关收发"的视角覆盖它，与 sceneServer 侧的用例互补）
// ---------------------------------------------------------------------------

TEST(BufferAppendRetrieve)
{
    Buffer b;
    b.append("hello", 5);
    b.append("world", 5);
    CHECK_EQ(b.readableBytes(), (size_t)10);

    CHECK(b.readAsString(5) == "hello");
    CHECK_EQ(b.readableBytes(), (size_t)5);

    b.retrieve(2);
    CHECK_EQ(b.readableBytes(), (size_t)3);
    CHECK(std::string(b.peek(), 3) == "rld");

    b.retrieveAll();
    CHECK_EQ(b.readableBytes(), (size_t)0);
}

// 半包/粘包：分两次 append，第一次不足以构成一条完整消息时不能消费
TEST(BufferPartialThenComplete)
{
    Buffer b;
    b.append("abc", 3);
    CHECK_EQ(b.readableBytes(), (size_t)3);

    // 模拟"半包"：只有 3 字节，业务侧应等待更多数据（这里只验证缓冲语义）
    b.append("defgh", 5);
    CHECK_EQ(b.readableBytes(), (size_t)8);
    CHECK(b.readAsString(8) == "abcdefgh");
    CHECK_EQ(b.readableBytes(), (size_t)0);
}

// prepend：帧头是"往前插"的，因此必须保证预留空间与字节序正确
TEST(BufferPrependNetworkOrder)
{
    Buffer b;
    b.append("BODY", 4);

    b.prependInt16(0xC1EA);   // 魔数（大端）
    b.prependInt32(0x01020304);
    b.prependInt64(0x1122334455667788ULL);

    CHECK_EQ(b.readableBytes(), (size_t)18);
    CHECK_EQ(b.readUint64(), (uint64_t)0x1122334455667788ULL);
    CHECK_EQ(b.readUint32(), (uint32_t)0x01020304);
    CHECK_EQ(b.readUint16(), (uint16_t)0xC1EA);
    CHECK(std::string(b.peek(), 4) == "BODY");
}

// 预留空间不足时必须能安全扩容（不能断言失败/越界写）
TEST(BufferPrependAfterPartialRead)
{
    Buffer b;
    b.append("0123456789", 10);
    b.retrieve(8);   // 可读只剩 2 字节，prepend 空间被消耗

    CHECK_EQ(b.readableBytes(), (size_t)2);

    std::string header(16, 'H');
    b.prepend(header.data(), header.size());

    CHECK_EQ(b.readableBytes(), (size_t)18);
    CHECK(std::string(b.peek(), 16) == header);
    CHECK(std::string(b.peek() + 16, 2) == "89");
}
