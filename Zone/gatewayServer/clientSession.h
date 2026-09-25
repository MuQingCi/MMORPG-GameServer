#ifndef CLEARMOON_GATEWAY_CLIENT_SESSION_H
#define CLEARMOON_GATEWAY_CLIENT_SESSION_H

#include "common/callbacks.h"   // TcpConnectionPtr

#include <cstdint>
#include <mutex>
#include <unordered_map>

/**
 * @brief 客户端会话
 *
 * 与"连接"的区分（不要把两者混成一个 id）：
 *   - 连接（TcpConnection）是**本进程内**的一条 TCP 链路；
 *   - 会话（ClientSession）是**公网客户端在本网关上的身份**，跨连接存在（重连后是新会话，
 *     epoch 递增），也是后端服务回包时用的跨进程回程地址的一部分
 */
struct ClientSession
{
    uint64_t sessionId  = 0;    // 网关内唯一（单调自增）
    uint32_t epoch      = 0;    // 该玩家在本网关的会话世代：鉴权（绑玩家）时递增
    uint64_t playerId   = 0;    // 0 = 未鉴权；非 0 之后才允许业务请求
    uint8_t  dstService = 0;    // 后续业务请求默认投递的后端服务号
    bool     authed     = false;

    TcpConnectionPtr conn;
    int64_t  createdAtMs = 0;
    int64_t  authedAtMs  = 0;
};

/**
 * @brief 客户端会话表 + 玩家索引
 *
 * 线程模型：与 ServiceRegistry 相同（baseLoop 与 IO 线程都可能访问），接口内部加锁，
 * 返回值一律是拷贝
 *
 * 玩家索引存在的意义：
 *   1) 后端服务的"主动推送"只带 playerId，必须能定位到当前会话（这是回程路由的第二条路径）；
 *   2) 同一玩家重复登录（顶号）时必须能立刻找到旧会话并踢掉，否则旧连接会继续收到推送
 */
class ClientSessionTable
{
public:
    // 新连接 -> 新会话（未鉴权）,epoch 从 0 开始，鉴权时递增
    uint64_t Create(const TcpConnectionPtr& conn, uint8_t defaultDstService, int64_t nowMs);

    bool Get(uint64_t sessionId, ClientSession& out) const;
    bool FindByPlayer(uint64_t playerId, ClientSession& out) const;

    /**
     * @brief 鉴权成功：把 playerId 绑到会话上，并递增该玩家的 epoch
     * @param kickedSessionId 被顶号的旧会话（0 = 没有）,调用方负责关闭旧连接并清掉它的回程路由
     * @return false = sessionId 不存在（连接已断）
     */
    bool Bind(uint64_t sessionId, uint64_t playerId, uint8_t dstService, int64_t nowMs,
              uint64_t& kickedSessionId);

    bool SetDstService(uint64_t sessionId, uint8_t dstService);

    // 连接关闭：移除会话与玩家索引（只允许移除"仍指向该会话"的索引，避免误删新会话）
    bool Erase(uint64_t sessionId);

    size_t Size() const;

private:
    mutable std::mutex mtx_;
    std::unordered_map<uint64_t, ClientSession> sessions_;
    std::unordered_map<uint64_t, uint64_t> playerIndex_;   // playerId -> sessionId
    std::unordered_map<uint64_t, uint32_t> playerEpoch_;   // playerId -> 最近分配的 epoch
    uint64_t nextSessionId_ = 1;
};

#endif
