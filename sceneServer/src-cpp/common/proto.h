#ifndef CLEARMOON_COMMON_PROTO_H
#define CLEARMOON_COMMON_PROTO_H

#include "buffer.h"
#include <cstdint>
#include <string>

static constexpr uint16_t kMagic           = 0xC1EA;
static constexpr uint16_t kHeaderSize      = 32;
static constexpr uint32_t kMaxMessageSize  = 10 * 1024 * 1024;//10MB
static constexpr uint32_t kVersion         = 1;

/**网络传输协议为
头部32Bytes
---------------------------------------------------------------------------------------------
|2B魔数|2B版本|1B网络消息类型|1B重试标记|2B模块|2B方法|8B序列号|8B玩家ID|4B总长度|1BSrc服务ID |1BDst服务ID|业务数据|
---------------------------------------------------------------------------------------------
其中魔数、版本号、总长度都用于decode校验;32位数字中高16位为模块号，低16位为方法号;
序列号、玩家ID、业务数据都用于decode后封装为Msg
网络消息类型用于decode参数types筛选特定消息
*/

enum NetMessageType : uint8_t
{
    kGatewayMsg = 1,
    kDBMsg      = 2,
    kGlobalMsg  = 3,
    kMessageMsg = 4,
    kSceneMsg   = 10
};

enum ServerID : uint8_t
{
    kGateway   = 1,
    kDB        = 2,
    kMessage   = 3,
    kGlobal    = 4,
    kScene_1   = 8
};

struct Header
{
    uint8_t retrFlag;
    uint16_t module;
    uint16_t method;
    
    uint64_t playerId;
    uint64_t seq;
    uint8_t  srcServiceID;
    uint8_t  dstServiceID;
};

void encode(Buffer& buf, uint8_t msgTypes, Header& head);
int decode(Buffer& buf, Header& head, uint8_t msgTypes, std::string& body);

#endif