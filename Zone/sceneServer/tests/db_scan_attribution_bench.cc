#include "db/taskDelivery.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <time.h>

namespace {
uint64_t cpuNs() {
    timespec t{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)!=0) std::abort();
    return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
uint64_t wallNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
Msg request(uint64_t id) {
    Msg m; m.head.taskId=id; m.head.msgType=MSGTYPE_DB_TASK_MYSQL;
    m.head.playerId=id; m.body=std::string(512,'s'); return m;
}
}

int main(int argc, char** argv)
{
    bool indexed=false;
    if(argc!=1) {
        if(argc!=3 || std::string(argv[1])!="--mode" ||
           (std::string(argv[2])!="legacy" && std::string(argv[2])!="indexed")) return 2;
        indexed=std::string(argv[2])=="indexed";
    }
    constexpr uint64_t rounds=2000,active=32;
    const auto accepted=[](const Msg&) { return DispatchResult::kAccepted; };
    const auto blocked=[](const Msg&) { return DispatchResult::kBackpressure; };
    // 每轮重复反转历史顺序；所有准备工作均在计时区间外完成。
    for(int repetition=0;repetition<3;++repetition) {
        std::vector<size_t> historySizes{0,512,2048,4096,8192,16384};
        if(repetition%2) std::reverse(historySizes.begin(),historySizes.end());
        for(auto history:historySizes) {
            PendingDbTasks tasks;
            if(indexed) tasks.enableActiveIndex();
            for(uint64_t i=1;i<=history;++i) {
                auto m=request(i); assert(tasks.submit(m,0,accepted));
                m.head.msgType=MSGTYPE_DB_RESULT_MYSQL; assert(tasks.complete(m));
            }
            tasks.tickControls(0,accepted); // 在测量区间外，对所有历史记录统一确认一次。
            for(uint64_t i=history+1;i<=history+active;++i)
                assert(tasks.submit(request(i),0,accepted));
            const auto state=tasks.diagnosticSnapshot();
            assert(state[0]==history+active && state[2]==active && state[4]==history);
            // 活跃请求尚未到期（下次查询时间为 5000），不会执行任何回调。
            uint64_t callbacks=0;
            const auto countSend=[&](const Msg&) { ++callbacks; return DispatchResult::kAccepted; };
            for(int i=0;i<100;++i) { tasks.tick(1000,countSend);tasks.tickControls(1000,countSend); }
            const auto w0=wallNs(),c0=cpuNs();
            for(uint64_t i=0;i<rounds;++i) { tasks.tick(1000,countSend);tasks.tickControls(1000,countSend); }
            const auto c1=cpuNs(),w1=wallNs();
            assert(callbacks==0);
            PendingDbTasks::ScanProbe tick,controls;
            tasks.tick(1000,countSend,&tick);tasks.tickControls(1000,countSend,&controls);
            assert(tick.visited[1]==active && tick.visited[3]==(indexed?0:history));
            assert(controls.visited[1]==active && controls.visited[3]==(indexed?0:history));
            std::cout<<"{\"mode\":\""<<(indexed?"indexed":"legacy")<<"\",\"historyVisits\":"<<tick.visited[3]+controls.visited[3]
                <<",\"activeVisits\":"<<tick.visited[1]+controls.visited[1]
                <<",\"rep\":"<<repetition<<",\"history\":"<<history<<",\"active\":"<<active
                <<",\"rounds\":"<<rounds<<",\"callbacks\":"<<callbacks<<",\"cpuNs\":"<<c1-c0
                <<",\"wallNs\":"<<w1-w0<<"}"<<std::endl;
        }
    }
    // 到期任务用例验证回调被独立计量，包括 ACK 背压。
    PendingDbTasks tasks;
    if(indexed) tasks.enableActiveIndex();
    assert(tasks.submit(request(1),0,blocked));
    PendingDbTasks::ScanProbe tick;
    tasks.tick(100,accepted,&tick);
    assert(tick.callbacks==1 && tick.visited[0]==1);
    auto result=request(1);result.head.msgType=MSGTYPE_DB_RESULT_MYSQL;
    assert(tasks.complete(result));
    PendingDbTasks::ScanProbe controls;
    tasks.tickControls(0,blocked,&controls);
    assert(controls.callbacks==1 && tasks.find(1)->ackPending);
    tasks.tickControls(100,accepted);
    assert(!tasks.find(1)->ackPending);
}