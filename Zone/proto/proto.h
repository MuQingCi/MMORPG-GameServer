#ifndef CLEARMOON_PROTO_H
#define CLEARMOON_PROTO_H

#include <cstdint>

//帧头部
using MAGIC         = uint16_t;
using VERSION       = uint16_t;

using MODULE        = uint16_t;
using METHOD        = uint16_t;
using LENGTH        = uint32_t;

//区服ID,用于区分各个区服组件
using ZoneID        = uint16_t;

using ServiceType   = uint16_t;
//实例ID
using InstanceID    = uint32_t;
//会话号
using SessionID     = uint64_t;
//会话世代
using EpochID       = uint32_t;

enum BackendService : uint8_t
{
    Gateway = 1,
    Scene   = 2,
    Global  = 3,
    Chat    = 4
};

//消息类型
enum MessageKind : uint8_t
{
    Request  = 1, // 请求，必须携带非零 requestId
    Response = 2, // 正常响应，回传请求的 requestId
    Push     = 3, // 服务端主动推送，可以没有 requestId
    Error    = 4, // 明确的错误响应
    Control  = 5, // 鉴权、握手、心跳、节点摘除等控制消息
};




#endif