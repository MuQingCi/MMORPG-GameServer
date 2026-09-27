#include "common/msg.h"
#include "common/msgBus.h"
#include "logic/logicThread.h"
#include "test_util.h"

#include <chrono>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// LogicThread 消息分发顺序（阶段 1 任务 1.16 / 1.18 的回归守卫）
//
// 测法：只靠**真实线程 + 真实消息投递**，不用任何桩：
//   - 构造一个 luaDir 指向不存在目录的 LogicThread（Lua 未就绪）；
//   - 通过 MsgBus::sendToWorker 投消息，等价于网络线程的投递路径；
//   - 用 playerCount() 观察 Actor 是否被创建（1.18 之后它是原子快照，跨线程读安全）。
//
// 为什么不依赖 Lua 脚本：1.16 的落点是"**路由存在**就创建 Actor"（见 onNetMsg 注释），
// 所以"合法路由"用例在 Lua 未就绪时同样会创建 Actor —— 用例稳定、不需要脚本环境。
// ---------------------------------------------------------------------------
namespace
{
LogicThread::Config TestCfg()
{
    LogicThread::Config cfg;
    cfg.luaDir = "/nonexistent_lua_dir_used_by_unit_test";
    cfg.idleWaitMs = 5;
    cfg.flushIntervalMs = 600000;   // 抬高：避免测试期间触发周期落盘
    cfg.scanIdleMs = 600000;        // 抬高：避免空闲扫描干扰
    cfg.luaInstructionLimit = 0;
    return cfg;
}

template <typename Fn>
bool WaitUntil(Fn&& fn, int timeoutMs = 1500)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (fn())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fn();
}

Msg MakeConnData(uint16_t module, uint16_t method, uint64_t playerId)
{
    Msg m;
    m.head.msgType = MsgType::MSGTYPE_CONN_DATA;
    m.head.Module = module;
    m.head.Method = method;
    m.head.session = 1001;
    m.head.seq = 7;
    m.head.playerId = playerId;
    return m;
}
}  // namespace

// 1.16 的回归点：无效路由（Module/Method 不在路由表）**不得**创建 Actor。
// 修前 onNetMsg 先 ensureActor 再判路由 → 这类垃圾消息会凭空造出 Actor（players=1）。
TEST(LogicThreadInvalidRouteDoesNotCreateActor)
{
    MsgBus bus(1);
    LogicThread lt(0, bus, TestCfg());
    CHECK(lt.start());

    CHECK(bus.sendToWorker(0, MakeConnData(9999, 9999, 4242)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    // 换一个 playerId 再来一次：仍然不创建
    CHECK(bus.sendToWorker(0, MakeConnData(8888, 8888, 5555)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    lt.stop();
}

// 正向对照：路由存在（即使 Lua 未就绪）→ 仍应创建 Actor。
// 这条用例同时防止"一刀切不创建 Actor"式的过度修复。
TEST(LogicThreadValidRouteStillCreatesActor)
{
    MsgBus bus(1);
    LogicThread lt(0, bus, TestCfg());
    CHECK(lt.start());

    CHECK(bus.sendToWorker(0, MakeConnData(Module::PLAYER, Method::PLAYER_MOVE, 4242)));
    CHECK(WaitUntil([&] { return lt.playerCount() == 1; }));

    lt.stop();
}

// 连接关闭必须回收 Actor（1.18 的快照也要跟着回落，否则 stats 会永远虚高）
TEST(LogicThreadConnCloseRemovesActor)
{
    MsgBus bus(1);
    LogicThread lt(0, bus, TestCfg());
    CHECK(lt.start());

    CHECK(bus.sendToWorker(0, MakeConnData(Module::PLAYER, Method::PLAYER_MOVE, 4242)));
    CHECK(WaitUntil([&] { return lt.playerCount() == 1; }));

    Msg closeMsg;
    closeMsg.head.msgType = MsgType::MSGTYPE_CONN_CLOSE;
    closeMsg.head.session = 1001;
    closeMsg.head.playerId = 4242;
    CHECK(bus.sendToWorker(0, std::move(closeMsg)));
    CHECK(WaitUntil([&] { return lt.playerCount() == 0; }));

    lt.stop();
}

// 无效路由的垃圾消息也不该刷新"活跃时间"（否则攻击者可用垃圾消息让空闲回收永不生效）。
// 这里用"创建/不创建"间接覆盖：Actor 都不存在，自然也就不存在被刷新的活跃度。
TEST(LogicThreadInvalidRouteKeepsNoState)
{
    MsgBus bus(1);
    LogicThread lt(0, bus, TestCfg());
    CHECK(lt.start());

    for (uint64_t pid = 10000; pid < 10005; ++pid)
        CHECK(bus.sendToWorker(0, MakeConnData(7777, 7777, pid)));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(lt.playerCount(), (size_t)0);

    lt.stop();
}
