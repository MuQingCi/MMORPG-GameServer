#include "common/msg.h"
#include "common/msgBus.h"
#include "test_util.h"
#include "timer/TimerThread.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace
{
// 等条件成立，最多等 timeoutMs；用于定时器这类异步行为
template<typename Pred>
bool WaitFor(Pred pred, int timeoutMs = 1000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}
}  // namespace

// 到期事件必须回到 ownerWorkerId 那一侧，且 ctx/playerId/epoch 原样透传
TEST(TimerFiresToOwnerWithCtx)
{
    MsgBus bus(2);
    TimerThread timer(bus, 5);   // 5ms tick，测试快一些
    CHECK(timer.start());

    TimerOp op;
    op.kind = TimerOp::ADD;
    op.timerId = 1001;
    op.ownerWorkerId = 1;
    op.intervalMs = 20;
    op.Module = Module::TIMER;
    op.Method = Method::TIMER_LUA_FIRE;
    op.playerId = 42;
    op.session = 7;
    op.epoch = 3;
    CHECK(timer.addTimer(op));

    Msg m;
    const bool got = WaitFor([&] { return bus.tryPopWorker(1, m); }, 2000);
    CHECK(got);
    if (got)
    {
        CHECK_EQ(m.head.msgType, (uint16_t)MsgType::MSGTYPE_TIMER_FIRE);
        CHECK_EQ(m.head.ctx, (uint64_t)1001);
        CHECK_EQ(m.head.playerId, (uint64_t)42);
        CHECK_EQ(m.head.session, (uint64_t)7);
        CHECK_EQ((uint32_t)m.head.epoch, (uint32_t)3);
        CHECK_EQ(m.head.srcWorkerId, (uint32_t)1);
    }

    timer.stop();
    timer.join();
}

// 取消后不能再触发；重复定时器要能连续触发多次
TEST(TimerCancelAndRepeat)
{
    MsgBus bus(2);
    TimerThread timer(bus, 5);
    CHECK(timer.start());

    // 重复定时器
    TimerOp rep;
    rep.kind = TimerOp::ADD;
    rep.timerId = 2001;
    rep.ownerWorkerId = 0;
    rep.intervalMs = 10;
    rep.repeat = true;
    rep.Module = Module::TIMER;
    rep.Method = Method::TIMER_LUA_FIRE;
    CHECK(timer.addTimer(rep));

    std::atomic<int> fired{0};
    Msg m;
    WaitFor(
        [&] {
            while (bus.tryPopWorker(0, m))
                fired.fetch_add(1);
            return fired.load() >= 3;
        },
        2000);
    CHECK(fired.load() >= 3);

    // 取消后计数不再增长
    CHECK(timer.cancel(2001));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    while (bus.tryPopWorker(0, m))
        fired.fetch_add(1);
    const int afterCancel = fired.load();

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    while (bus.tryPopWorker(0, m))
        fired.fetch_add(1);
    CHECK_EQ(fired.load(), afterCancel);

    // 一次性定时器：取消后一次都不该触发
    TimerOp once;
    once.kind = TimerOp::ADD;
    once.timerId = 2002;
    once.ownerWorkerId = 0;
    once.intervalMs = 50;
    once.Module = Module::TIMER;
    once.Method = Method::TIMER_LUA_FIRE;
    CHECK(timer.addTimer(once));
    CHECK(timer.cancel(2002));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool gotCancelled = false;
    while (bus.tryPopWorker(0, m))
    {
        if (m.head.ctx == 2002)
            gotCancelled = true;
    }
    CHECK(!gotCancelled);

    timer.stop();
    timer.join();
}

// 停止后不得再有新触发（退出路径要干净）
TEST(TimerStopClearsPending)
{
    MsgBus bus(1);
    TimerThread timer(bus, 5);
    CHECK(timer.start());

    TimerOp op;
    op.kind = TimerOp::ADD;
    op.timerId = 3001;
    op.ownerWorkerId = 0;
    op.intervalMs = 30;
    op.Module = Module::TIMER;
    op.Method = Method::TIMER_LUA_FIRE;
    CHECK(timer.addTimer(op));

    timer.stop();
    timer.join();

    Msg m;
    while (bus.tryPopWorker(0, m)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool fired = false;
    while (bus.tryPopWorker(0, m))
        fired = true;
    CHECK(!fired);
}
