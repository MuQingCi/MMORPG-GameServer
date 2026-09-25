#ifndef CLEARMOON_SCENESERVER_H
#define CLEARMOON_SCENESERVER_H

#include "config/sceneConfig.h"
#include "common/msgBus.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class NetServer;
class TimerThread;
class DBThread;
class LogicThread;

/**
 * @brief 场景服进程的组装者与生命周期管理者
 *
 * 依赖关系--决定了创建与停止的顺序：
 *
 *   MsgBus  <- 谁都要用它投消息，必须最先创建、最后销毁
 *     ├── NetServer    投 CONN_* 给逻辑线程；消费 MSGTYPE_SEND
 *     ├── TimerThread  到期回投 ownerWorkerId
 *     ├── DBThread     结果回投 srcWorkerId
 *     └── LogicThread × N  只持有 lua_State 与自己的玩家分片
 *
 * 启动顺序：bus → net/timer/db → bindTimer → logic threads → 反向连接网关
 * 停止顺序：net（不再有新输入） → logic → timer → db → bus
 *   —— 必须先停"生产者"再停"消费者"，否则会出现"结果回来时消费者已经没了"。
 */
class SceneServer
{
public:
    explicit SceneServer(const SceneConfig& cfg);
    ~SceneServer();

    SceneServer(const SceneServer&) = delete;
    SceneServer& operator=(const SceneServer&) = delete;

    bool start();
    void stop();

    // 热更：把"版本号 + 新目录"广播给全部逻辑线程。
    // 每个 worker 在安全点独立应用（版本号不会被先完成的 worker 清掉）。
    // 目录即版本：发布后目录内容永不修改，出问题可指回旧目录回滚。
    void reloadLua(const std::string& newDir);

    const SceneConfig& config() const { return cfg_; }
    MsgBus& bus() { return *bus_; }

    // 监控快照（供 main 周期打印）
    std::string stats() const;
    size_t playerCount() const;

private:
    SceneConfig cfg_;

    std::unique_ptr<MsgBus> bus_;
    std::unique_ptr<NetServer> netServer_;
    std::unique_ptr<TimerThread> timerThread_;
    std::unique_ptr<DBThread> dbThread_;
    std::vector<std::unique_ptr<LogicThread>> logicThreads_;

    std::atomic<uint64_t> luaVersion_{0};
    std::atomic<bool> started_{false};
    // stop() 的幂等标志：start() 中途失败时也必须能停掉已创建的部分
    std::atomic<bool> stopped_{false};
    std::string luaDir_;   // 当前生效的脚本目录（热更会替换）
};

#endif
