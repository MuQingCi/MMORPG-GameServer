#ifndef CLEARMOON_DB_REDISCONNECTION_H
#define CLEARMOON_DB_REDISCONNECTION_H

#include "db/dbConfig.h"
#include "db/dbValue.h"

#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief Redis 连接（**每个 DB 线程独占一个实例**）
 *
 * 为什么 Redis 必须与 MySQL 分通道：
 *   Redis 是亚毫秒级、MySQL 是毫秒到几十毫秒级。共用一个队列/线程时，
 *   一条慢 SQL 会把后面所有 Redis 读全部堵死（设计文档 3.5）。
 *   这里与 MySQL 通道完全对称：各自线程、各自连接、各自队列。
 *
 * key 空间约定（区服唯一共享 Redis）：
 *   zone:{zoneId}:scene:{sceneId}:player:{playerId}:xxx
 *   只用 key 前缀做隔离，不用 SELECT 切换 database index
 *   —— 后者会让将来迁移到 Redis Cluster 变得不可能。
 *
 * task.args 就是一条完整命令的数组：{"HSET", "k", "f", "v"}
 * （不做 SQL 式解析，Redis 本来就是命令数组协议）
 */
class RedisConnection
{
public:
    RedisConnection();
    ~RedisConnection();

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    bool Connect(const RedisConfig& cfg);
    void Close();
    bool IsConnected() const;

    /**
     * @brief 执行一条 Redis 命令
     * @param out.errcode 0 表示成功；
     *        data 是回复的值树：status/int/string -> 对应 Val；
     *        数组回复 -> Val::Arr（元素递归）
     */
    void Execute(const dbv::DbTask& task, dbv::DbResult& out);

    bool Reconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RedisConfig cfg_;
};

#endif
