#ifndef CLEARMOON_LOGIC_LOGICTHREAD_H
#define CLEARMOON_LOGIC_LOGICTHREAD_H

#include "common/queue.h"
#include "common/msgBus.h"


#include <cstdint>
#include <thread>

struct lua_State;

class LogicThread
{
public:
    LogicThread(uint32_t threadId, MsgBus& bus);
    ~LogicThread();

    bool start();
    void stop();

    void run();
private:
    void work();

    std::thread thread_;
    uint32_t threadId_;
    bool stared_ = false;

    lua_State* l_state = nullptr;
    MsgBus* m_bus_ = nullptr;
};  

#endif