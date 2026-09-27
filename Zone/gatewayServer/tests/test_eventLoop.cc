#include "event/eventLoop.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// Reactor 核心：同线程立即执行、跨线程投递、quit 退出
//   EventLoop 是线程亲和的：runInLoop/queueInLoop 在"本线程"与"别的线程"下
//   行为不同，这正是网关把连接分派到多个 IO 线程的基础。
// ---------------------------------------------------------------------------

// 同线程：runInLoop 立即执行；queueInLoop 也应在当前事件处理结束后执行
TEST(EventLoopRunInLoopExecutesImmediately)
{
    EventLoop loop;

    CHECK(loop.isInThread());

    bool ran = false;
    loop.runInLoop([&ran] { ran = true; });
    CHECK(ran);   // 同线程时 runInLoop 是就地调用，不需要等 loop()

    // queueInLoop 的语义是"延迟到本轮事件处理结束后执行"。
    // ★ 易踩点（本用例同时固化）：loop() 内部是 poll(-1) 永久阻塞，而**同线程**
    //   queueInLoop 不会触发 weakup()（只有跨线程或正在处理 pending 时才会），
    //   所以事件循环若处于空闲阻塞，必须显式 weakup() 才能让队列被处理。
    bool queuedRan = false;
    loop.queueInLoop([&] {
        queuedRan = true;
        loop.quit();
    });
    CHECK(!queuedRan);   // 尚未进入事件循环

    loop.weakup();        // 显式唤醒（跨线程 queueInLoop 时内部会自己 weakup）
    loop.loop();          // 处理队列并因 quit() 返回
    CHECK(queuedRan);
}

// 跨线程：别的线程 queueInLoop 必须能唤醒 loop() 并被执行
TEST(EventLoopQueueInLoopFromOtherThread)
{
    EventLoop loop;

    std::atomic<bool> otherSawNotInThread{false};
    std::atomic<bool> taskRan{false};

    std::thread t([&] {
        otherSawNotInThread.store(!loop.isInThread());
        loop.queueInLoop([&] {
            taskRan.store(true);
            loop.quit();
        });
    });

    loop.loop();   // 阻塞，直到上面的任务执行并 quit()
    t.join();

    CHECK(otherSawNotInThread.load());
    CHECK(taskRan.load());
}

// queueInLoop 的任务不在当前调用栈里执行，而是等本轮事件处理结束后执行
// （注意同线程场景需要显式 weakup，原因见上一条用例的注释）
TEST(EventLoopQueueInLoopIsDeferredInSameThread)
{
    EventLoop loop;

    bool ranInsideLoop = false;
    loop.queueInLoop([&] {
        ranInsideLoop = true;
        loop.quit();
    });

    loop.weakup();
    loop.loop();
    CHECK(ranInsideLoop);
}
