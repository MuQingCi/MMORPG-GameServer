#ifndef CLEARMOON_SCENESERVER_H
#define CLEARMOON_SCENESERVER_H


#include <memory>
class SceneServer
{
public:


private:
    std::unique_ptr<MsgBus> bus_;
};

#endif