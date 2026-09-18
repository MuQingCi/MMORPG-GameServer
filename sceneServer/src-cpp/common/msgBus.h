#ifndef CLEARMOON_COMMON_MSGBUS_H
#define CLEARMOON_COMMON_MSGBUS_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <chrono>

class Msg;

template<typename T>
class MsgQueue;

class MsgBus
{
public:
using ActorId  = uint64_t;
using ModuleId = uint32_t;
using WorkerId = uint32_t;
using MsgPtr = std::shared_ptr<Msg>;  //选择shared_ptr以适配broadcastToLogic
using PlayerId = uint64_t;
    
    explicit MsgBus(size_t num_workers);
    ~MsgBus();
    MsgBus(const MsgBus&) = delete;
    MsgBus& operator=(const MsgBus&) = delete;
    MsgBus(MsgBus&&) = delete;
    MsgBus& operator=(MsgBus&&) = delete;
    
    //根据第一个参数哈希
    bool sendToActor(ActorId id, Msg m);
    bool sendToModule(ModuleId mid, Msg m);
    bool sendToWorkerByPlayerId(PlayerId pid, Msg m);

    //精准回投源逻辑线程
    bool sendToWorker(WorkerId wid, Msg m);
    //投递消息到对应辅助线程
    bool sendToNet(Msg m);
    bool sendToDB(Msg m);

    //从辅助线程对应的消息队列中取消息
    bool tryPopWorker(WorkerId wid, Msg& out);
    bool tryPopNet(Msg& out);
    bool tryPopDB(Msg& out);

    //超时等待版本
    bool waitPopWorker(WorkerId wid, Msg& out,std::chrono::milliseconds timeoutMs);
    bool waitPopNet(Msg& out, std::chrono::milliseconds timeoutMs);
    bool waitPopDB(Msg& out, std::chrono::milliseconds timeoutMs);

    void broadcastToLogic(MsgPtr m);

    //获取当前逻辑线程数
    size_t numWorker() const { return worker_queues_.size(); }
    //当目标Work队列为空时等待其非空或超timeroutMs返回
    void waitWorker(WorkerId wid, std::chrono::milliseconds timeoutMs);
private:
    std::vector<std::unique_ptr<MsgQueue<Msg>>> worker_queues_;
    std::unique_ptr<MsgQueue<Msg>> net_queue_;
    std::unique_ptr<MsgQueue<Msg>> db_queue_;
};
#endif