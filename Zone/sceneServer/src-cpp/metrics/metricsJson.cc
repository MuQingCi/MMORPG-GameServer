#include "metrics/metricsJson.h"

#include <sstream>

std::string serializeSceneMetrics(const SceneMetricsSnapshot& snapshot)
{
    std::ostringstream out;
    out << "{\"schemaVersion\":2,\"sampleValid\":" << (snapshot.sampleValid ? 1 : 0)
        << ",\"sampleElapsedMs\":" << snapshot.sampleElapsedMs
        << ",\"sampleSequence\":" << snapshot.sampleSequence
        << ",\"logicThreads\":" << snapshot.workers.size()
        << ",\"uptimeMs\":" << snapshot.uptimeMs
        << ",\"sampledAtMs\":" << snapshot.sampledAtMs
        << ",\"netQueue\":" << snapshot.netQueueDepth
        << ",\"playerCount\":" << snapshot.playerCount
        << ",\"handled\":" << snapshot.totalHandled
        << ",\"windowHandled\":" << snapshot.windowHandled
        << ",\"handledPerSec\":" << snapshot.handledPerSecond
        << ",\"handlerUs\":" << snapshot.totalHandlerUs
        << ",\"workerQueueDepth\":" << snapshot.totalQueueDepth
        << ",\"workerActivePermilleAverage\":" << snapshot.workerActivePermilleAverage
        << ",\"workerActivePermilleMax\":" << snapshot.workerActivePermilleMax
        << ",\"workerEquivalentCapacityPermille\":" << snapshot.workerEquivalentCapacity
        << ",\"suspectedStallWorkers\":" << snapshot.suspectedStallCount
        << ",\"overloadedWorkers\":" << snapshot.overloadedWorkerCount;
    for (size_t id = 0; id < snapshot.workers.size(); ++id)
    {
        const auto& w = snapshot.workers[id];
        out << ",\"worker" << id << "_queue\":" << w.queueDepth
            << ",\"worker" << id << "_handled\":" << w.handled
            << ",\"worker" << id << "_dropped\":" << w.dropped
            << ",\"worker" << id << "_busyUs\":" << w.busyUs
            << ",\"worker" << id << "_maintenanceUs\":" << w.maintenanceUs
            << ",\"worker" << id << "_cycleUs\":" << w.cycleUs
            << ",\"worker" << id << "_waitUs\":" << w.waitUs
            << ",\"worker" << id << "_idleWaitUs\":" << w.idleWaitUs
            << ",\"worker" << id << "_maxLoopUs\":" << w.maxLoopUs
            << ",\"worker" << id << "_maxLoopUsWindow\":" << w.maxLoopUsWindow
            << ",\"worker" << id << "_players\":" << w.players
            << ",\"worker" << id << "_enqueuedBusiness\":" << w.enqueuedBusiness
            << ",\"worker" << id << "_enqueuedInternal\":" << w.enqueuedInternal
            << ",\"worker" << id << "_dequeuedBusiness\":" << w.dequeuedBusiness
            << ",\"worker" << id << "_dequeuedInternal\":" << w.dequeuedInternal
            << ",\"worker" << id << "_health\":" << static_cast<unsigned>(w.health)
            << ",\"worker" << id << "_lifecycle\":" << static_cast<unsigned>(w.lifecycle)
            << ",\"worker" << id << "_luaReady\":" << (w.luaReady ? 1 : 0)
            << ",\"worker" << id << "_phase\":" << static_cast<unsigned>(w.phase)
            << ",\"worker" << id << "_handledPerSec\":" << w.handledPerSecond
            << ",\"worker" << id << "_activePermille\":" << w.activePermille
            << ",\"worker" << id << "_overloaded\":" << (w.overloaded ? 1 : 0)
            << ",\"worker" << id << "_lastProgressMs\":" << w.lastProgressMs
            << ",\"worker" << id << "_handlerMaxUsWindow\":" << w.handlerMaxUsWindow
            << ",\"worker" << id << "_cycleMaxUsWindow\":" << w.cycleMaxUsWindow;
        for (uint32_t type = 1; type <= 100; ++type)
            out << ",\"worker" << id << "_type" << type << "\":" << w.messageTypeCounts[type]
                << ",\"worker" << id << "_dequeuedType" << type << "\":" << w.dequeuedMessageTypeCounts[type];
        size_t index = 0;
        for (uint32_t method : {1, 2, 3, 5, 6})
            out << ",\"worker" << id << "_serviceMethod" << method << "\":" << w.serviceMethodCounts[index++];
    }
    out << "}\n";
    return out.str();
}