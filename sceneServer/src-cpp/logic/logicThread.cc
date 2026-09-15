#include "logicThread.h"
#include "msg.h"
#include <thread>

LogicThread::LogicThread(uint32_t threadId, MsgBus& bus)
                        :threadId_(threadId),
                         m_bus_(&bus)
{

}

LogicThread::~LogicThread()
{
    stop();
}

bool LogicThread::start()
{
    if(stared_) return true;

    thread_ = std::thread([this]{run();});
    stared_ = true;
    return true;
}

void LogicThread::stop()
{
    if(!stared_) return;

    if(thread_.joinable())
        thread_.join();
    stared_ = false;
}

void LogicThread::run()
{
    while (stared_) {
        Msg m;
        while(m_bus_->tryPopWorker(threadId_, m))
        {
            //TODO 根据消息查路由表并处理
        }
    }
}
