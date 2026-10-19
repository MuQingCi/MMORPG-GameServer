#include "metrics/aoiDiagnostics.h"
#include <cassert>
#include <thread>

int main()
{
    AoiDiagnostics metrics;
    metrics.duration(AoiDiagnostics::Stage::Query, 10, 9);
    assert(metrics.snapshot().invalidTimes == 1);
    metrics.duration(AoiDiagnostics::Stage::Query, 10, 110);
    metrics.duration(AoiDiagnostics::Stage::Query, 10, 100011);
    metrics.add(AoiDiagnostics::Counter::Visible, 7);
    auto snapshot = metrics.snapshot();
    const auto& query = snapshot.stages[0];
    assert(query.count == 2 && query.sumUs == 100101 && query.maxUs == 100001);
    assert(query.bins[0] == 1 && query.bins[9] == 1);
    assert(snapshot.counters[static_cast<size_t>(AoiDiagnostics::Counter::Visible)] == 7);
    std::thread writer([&] {
        for (int i = 0; i < 10000; ++i)
            metrics.duration(AoiDiagnostics::Stage::Commit, 1, 2);
    });
    for (int i = 0; i < 1000; ++i) metrics.snapshot();
    writer.join();
    assert(metrics.snapshot().stages[4].count == 10000);
    { AoiDiagnostics::Scope disabled(nullptr, AoiDiagnostics::Stage::Query); }
    assert(metrics.snapshot().stages[0].count == 2);
}