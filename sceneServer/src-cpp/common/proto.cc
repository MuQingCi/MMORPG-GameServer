#include "proto.h"
#include "log/logger.h"
#include "utils.h"

#include <cstdint>
#include <cstring>
#include <algorithm>


/**
 * @brief 只窥探Buffer中的数据，不修改其中数据
 * 
 * @param buff 
 * @return size_t 
 */
static size_t findMagicInBuffer(Buffer* buff)
{
    if(buff == nullptr) return static_cast<size_t>(-1);

    //将魔数转换成大端字节序并分别保留其高位/低位字节以使用search寻找相同子序列
    uint16_t magicNet = host16ToNet(kMagic);
    char magicBytes[2];
    magicBytes[0] = static_cast<char>((magicNet >>8) & 0xFF);
    magicBytes[1] = static_cast<char>(magicNet & 0xFF);

    const char* start = buff->peek();
    const char* end = start + buff->readableBytes();
    const char* pos = std::search(start,end,magicBytes,magicBytes + 2);

    if(pos == end)
        return static_cast<size_t>(-1);
    return static_cast<size_t>(pos - start);
}


void encode(Buffer& buf, uint8_t msgTypes, Header& head)
{
    uint32_t body_len = buf.readableBytes();

    buf.prependInt8(head.dstServiceID);
    buf.prependInt8(head.srcServiceID);

    buf.prependInt32(body_len);
    
    buf.prependInt64(head.playerId);
    buf.prependInt64(head.seq);

    buf.prependInt16(head.module);
    buf.prependInt16(head.method);
    buf.prependInt8(head.retrFlag);

    buf.prependInt8(msgTypes);

    buf.prependInt16(kVersion);
    buf.prependInt16(kMagic);
}

int decode(Buffer& buf, Header& head, uint8_t msgTypes, std::string& body)
{
    if(buf.readableBytes() <= kMinLength)
        return 0;
    
    size_t offset;
    while(buf.readableBytes() >= kMinLength)
    {
        uint16_t magicNet;
        std::memcpy(&magicNet, buf.peek(),sizeof(magicNet));
        uint16_t magic = netToHost16(magicNet);

        //在Buffer中寻找下一个正确的魔数
        offset = findMagicInBuffer(&buf);
        if(magic != kMagic)
        {
            if(offset == static_cast<size_t>(-1))
            {
                //并不存在一个正确的魔数,把所有数据包全部丢弃
                LOG_INFO<< "Discard "<< buf.readableBytes() <<" Bytes of data";
                buf.retrieveAll();
                return -1;
            }
            else {
                //将第一个正确的魔数之前的数据包全部丢弃
                buf.retrieve(offset);
                LOG_INFO<< "Discard "<< offset <<" Bytes of data";
                continue;
            }
        }
        uint16_t versionNet;
        std::memcpy(&versionNet, buf.peek() + sizeof(magic), sizeof(versionNet));
        uint16_t version = netToHost16(versionNet);

        uint8_t msgType;
        std::memcpy(&msgType, buf.peek()+4, sizeof(msgType));
        if(msgType != msgTypes)
        {
            if(offset == static_cast<size_t>(-1))
            {
                //并不存在一个正确的魔数,把所有数据包全部丢弃
                LOG_INFO<< "Discard "<< buf.readableBytes() <<" Bytes of data";
                buf.retrieveAll();
                return -1;
            }
            LOG_INFO<< "msgTypes:"<< msgTypes <<"Invalid";
            buf.retrieve(offset);
            continue;
        }

        uint32_t totalLenNet;
        std::memcpy(&totalLenNet, buf.peek() + 28, sizeof(totalLenNet));
        uint32_t totalLen = netToHost32(totalLenNet);

        //长度不合法，先读取一字节数据以防假魔数
        if(totalLen < kMinLength || totalLen > kMaxMessageSize)
        {
            buf.retrieve(1);
            LOG_INFO<< "Discard "<< 1 <<" Bytes of data";
            continue;
        }

        if(totalLen > buf.readableBytes())
        {
            //数据包不完全，等下次读取
            return 0;
     
        }

        auto Magic = buf.readUint16();
        auto Version = buf.readUint16();
        auto msgtype = buf.readUint8();
        head.retrFlag = buf.readUint8();
        head.module = buf.readUint16();
        head.method = buf.readUint16();
        head.seq = buf.readUint64();
        head.playerId = buf.readUint64();

        auto Body_len = buf.readUint32();
        head.srcServiceID = buf.readUint8();
        head.dstServiceID = buf.readUint8();

        if(buf.readableBytes() < Body_len) 
            return 0;

        body = buf.readAsString(Body_len);
        return 1;
    }
    return -1;
}