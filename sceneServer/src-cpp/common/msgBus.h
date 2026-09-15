#ifndef CLEARMOON_COMMON_MSGBUS_H
#define CLEARMOON_COMMON_MSGBUS_H

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

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
    
    explicit MsgBus(size_t num_workers);
    ~MsgBus();
    MsgBus(const MsgBus&) = delete;
    MsgBus& operator=(const MsgBus&) = delete;
    MsgBus(MsgBus&&) = delete;
    MsgBus& operator=(MsgBus&&) = delete;
    
    //根据第一个参数哈希
    void sendToActor(ActorId id, Msg& m);
    void sendToModule(ModuleId mid, Msg& m);
    //精准回投源逻辑线程
    void sendToWorker(WorkerId wid, Msg& m);
    //投递消息到对应辅助线程
    void sendToNet(MsgPtr m);
    void sendToDB(MsgPtr m);

    //从辅助线程对应的消息队列中取消息
    bool tryPopWorker(WorkerId wid, Msg& out);
    bool tryPopNet(Msg& out);
    bool tryPopDB(Msg& out);

    void broadcastToLogic(MsgPtr m);
private:
    std::vector<std::unique_ptr<MsgQueue<Msg>>> worker_queues_;
    std::unique_ptr<MsgQueue<Msg>> net_queue_;
    std::unique_ptr<MsgQueue<Msg>> db_queue_;
};
#endif