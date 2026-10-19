#include "metrics/queueDiagnosticsJson.h"
#include <sstream>

std::string serializeQueueDiagnostics(const QueueDiagnostics::Snapshot& snapshot)
{
    std::ostringstream out; out<<"{\"monotonicUs\":"<<snapshot.monotonicUs<<",\"netQueue\":"<<snapshot.netQueue<<",\"depths\":[";
    for(size_t i=0;i<snapshot.depths.size();++i) { if(i) out<<','; out<<snapshot.depths[i]; }
    out<<"],\"fullSnapshot\":"<<(snapshot.fullSnapshot?"true":"false")<<",\"writeFailed\":"<<(snapshot.writeFailed?"true":"false")<<",\"admissions\":[";
    for(size_t i=0;i<snapshot.admissions.size();++i) {
        if(i) out<<',';
        out<<'[';
        for(size_t j=0;j<5;++j) { if(j) out<<','; out<<snapshot.admissions[i][j]; } out<<']';
    }
    out<<"],\"waits\":[";
    for(size_t i=0;i<snapshot.waits.size();++i) for(size_t c=0;c<4;++c) {
        if(i||c) out<<',';
        const auto& w=snapshot.waits[i][c];
        out<<"{\"worker\":"<<i<<",\"category\":"<<c<<",\"count\":"<<w.count
           <<",\"sumUs\":"<<w.sumUs<<",\"maxUs\":"<<w.maxUs<<",\"missing\":"<<w.missing<<",\"bins\":[";
        for(size_t b=0;b<10;++b) { if(b) out<<','; out<<w.bins[b]; } out<<"]}";
    }
    out<<"],\"droppedEvents\":"<<snapshot.droppedEvents<<",\"events\":[";
    for(size_t i=0;i<snapshot.events.size();++i) {
        if(i) out<<',';
        const auto& e=snapshot.events[i];
        out<<"{\"worker\":"<<e.worker<<",\"kind\":\""<<e.kind<<"\",\"beginUs\":"<<e.begin
           <<",\"endUs\":"<<e.end<<",\"count\":"<<e.count<<'}';
    }
    out<<"],\"work\":[";
    auto duration=[&](const WorkDiagnostics::Duration& d) {
        out<<"{\"count\":"<<d.count<<",\"sumUs\":"<<d.sumUs<<",\"maxUs\":"<<d.maxUs<<",\"bins\":[";
        for(size_t i=0;i<d.bins.size();++i) { if(i) out<<','; out<<d.bins[i]; }
        out<<"]}";
    };
    for(size_t i=0;i<snapshot.work.size();++i) {
        if(i) out<<',';
        out<<"{\"classes\":[";
        for(size_t j=0;j<6;++j) {
            if(j) out<<',';
            const auto& c=snapshot.work[i].classes[j];
            out<<"{\"attempted\":"<<c.attempted<<",\"accepted\":"<<c.accepted<<",\"rejected\":"<<c.rejected<<",\"firstRejectedUs\":"<<c.firstRejectedUs<<",\"handled\":";
            duration(c.handled); out<<'}';
        }
        out<<"],\"phases\":[";
        for(size_t j=0;j<snapshot.work[i].phases.size();++j) { if(j) out<<','; duration(snapshot.work[i].phases[j]); }
        const auto& w=snapshot.work[i];
        out<<"],\"dbSampleUs\":"<<w.dbSampleUs<<",\"dbTasks\":[";
        for(size_t j=0;j<w.dbTasks.size();++j) { if(j) out<<','; out<<w.dbTasks[j]; }
        out<<"],\"dbScanned\":["<<w.dbScanned[0]<<','<<w.dbScanned[1]<<"],\"netPending\":[";
        for(size_t j=0;j<w.netPending.size();++j) { if(j) out<<','; out<<w.netPending[j]; }
        out<<"],\"dbProbe\":[";
        for(size_t p=0;p<w.dbProbe.size();++p) {
            if(p) out<<',';
            out<<'[';
            for(size_t j=0;j<w.dbProbe[p].size();++j) { if(j) out<<','; out<<w.dbProbe[p][j]; }
            out<<']';
        }
        out<<"]}";
    }
    out<<"],\"aoi\":[";
    for(size_t i=0;i<snapshot.aoi.size();++i) {
        if(i) out<<',';
        const auto& a=snapshot.aoi[i];
        out<<"{\"invalidTimes\":"<<a.invalidTimes<<",\"stages\":[";
        for(size_t j=0;j<a.stages.size();++j) {
            if(j) out<<',';
            const auto& d=a.stages[j];
            out<<"{\"count\":"<<d.count<<",\"sumUs\":"<<d.sumUs<<",\"maxUs\":"<<d.maxUs<<",\"bins\":[";
            for(size_t k=0;k<d.bins.size();++k) { if(k) out<<','; out<<d.bins[k]; }
            out<<"]}";
        }
        out<<"],\"counters\":[";
        for(size_t j=0;j<a.counters.size();++j) { if(j) out<<','; out<<a.counters[j]; }
        out<<"]}";
    }
    out<<"]}\n"; return out.str();
}

std::string QueueDiagnostics::json(uint64_t now, const std::vector<size_t>& depths, size_t net, bool full)
{
    return serializeQueueDiagnostics(collect(now, depths, net, full));
}
