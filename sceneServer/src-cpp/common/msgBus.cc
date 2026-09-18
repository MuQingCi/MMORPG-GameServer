#include "msgBus.h"
#include "log/logger.h"
#include "msg.h"
#include "queue.h"
#include <cassert>
#include <memory>
#include <utility>

namespace 
{
//Actor哈希规则

//Module哈希规则

}

MsgBus::MsgBus(size_t num_workers) : net_queue_(std::make_unique<MsgQueue<Msg>>()),
                                     db_queue_(std::make_unique<MsgQueue<Msg>>())
{
    worker_queues_.reserve(num_workers);
    for(int i = 0; i < num_workers; ++i)
        worker_queues_.emplace_back(std::make_unique<MsgQueue<Msg>>());
}

MsgBus::~MsgBus() = default;

bool MsgBus::sendToActor(ActorId id, Msg m)
{
    //TODO
}

bool MsgBus::sendToModule(ModuleId mid, Msg m)
{
    //TODO
}

bool MsgBus::sendToWorkerByPlayerId(PlayerId pid, Msg m)
{
    auto hashWid = pid % worker_queues_.size();
    return sendToWorker(hashWid, std::move(m));
}

//精准回投源逻辑线程
bool MsgBus::sendToWorker(WorkerId wid, Msg m)
{
    if(wid >= worker_queues_.size())
    {
        LOG_ERROR<<"The wid >= workerNum";
        return false;
    }

    bool res = worker_queues_[wid]->try_push(std::move(m));
    return res;
}

//投递消息到对应辅助线程
bool MsgBus::sendToNet(Msg m)
{
    return net_queue_->try_push(std::move(m));;
}

bool MsgBus::sendToDB(Msg m)
{
    return db_queue_->try_push(std::move(m));;
}

//从辅助线程对应的消息队列中取消息
bool MsgBus::tryPopWorker(WorkerId wid, Msg& out)
{
    if(wid >= worker_queues_.size())
    {
        LOG_ERROR<<"The wid >= workerNum";
        return false;
    }

    return worker_queues_[wid]->try_pop(out);
}

bool MsgBus::tryPopNet(Msg& out)
{
    return net_queue_->try_pop(out);
}

bool MsgBus::tryPopDB(Msg& out)
{
    return db_queue_->try_pop(out);
}

//超时等待版本
bool MsgBus::waitPopWorker(WorkerId wid, Msg& out,std::chrono::milliseconds timeoutMs)
{
    if(wid >= worker_queues_.size())
    {
        LOG_ERROR<<"The wid >= workerNum";
        return false;
    }
    return worker_queues_[wid]->wait_pop_for(out, timeoutMs);
}

bool MsgBus::waitPopNet(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return net_queue_->wait_pop_for(out, timeoutMs);
}

bool MsgBus::waitPopDB(Msg& out, std::chrono::milliseconds timeoutMs)
{
    return db_queue_->wait_pop_for(out, timeoutMs);
}


void MsgBus::broadcastToLogic(MsgPtr m)
{
    if(!m) return;

    for(auto& q : worker_queues_)
    {
        Msg copy = *m;    
        if (!q->try_push(std::move(copy))) 
        {
            LOG_ERROR << "broadcastToLogic: try_push failed";
        }
    }
}