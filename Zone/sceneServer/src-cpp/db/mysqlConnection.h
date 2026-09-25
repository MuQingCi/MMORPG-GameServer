#ifndef CLEARMOON_DB_MYSQLCONNECTION_H
#define CLEARMOON_DB_MYSQLCONNECTION_H

#include "db/dbConfig.h"
#include "db/dbValue.h"

#include <cstdint>
#include <memory>
#include <string>

/**
 * @brief MySQL 连接（**每个 DB 线程独占一个实例**，非线程安全，也不需要线程安全）
 *
 * 为什么不做连接池 + 共享：
 *   MySQL 的 C API 连接对象不能跨线程并发使用。既然设计上 MySQL 通道只有
 *   一个消费线程，最简且最快的形态就是"线程独占连接"：
 *   无锁、无池化开销、无"归还时状态是否干净"的问题。
 *   将来需要更多并发时，扩的是 DB 线程数（每线程一连接），而不是给连接加锁。
 *
 * 为什么用 prepared statement：
 *   任务里的 args 是"SQL 模板 + 占位参数"（Lua 侧只暴露语义化 API，SQL 留在 C++）。
 *   参数化同时解决防注入与类型转换两件事，比手工拼串安全得多。
 *
 * 头文件里不出现 mysql.h：用 pimpl 把依赖隔离在 .cc，
 * 上层无需 -I/usr/include/mysql，也避免 MySQL 头里大量宏的污染。
 */
class MySqlConnection
{
public:
    MySqlConnection();
    ~MySqlConnection();

    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    bool Connect(const MySqlConfig& cfg);
    void Close();
    bool IsConnected() const;

    /**
     * @brief 执行一条任务
     * @param task.kind  1 = select（返回行集）, 2 = exec（返回[影响行数, 自增id]）
     * @param task.args  args[0] = SQL；args[1..] = 占位参数（可缺省）
     * @param out        结果；errcode != 0 表示失败，errmsg 带原因
     */
    void Execute(const dbv::DbTask& task, dbv::DbResult& out);

    // 断线重连（由 DB 线程在连接失效时调用，失败退避策略由调用方控制）
    bool Reconnect();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MySqlConfig cfg_;
};

#endif
