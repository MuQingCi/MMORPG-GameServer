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
    out<<"]}\n"; return out.str();
}

std::string QueueDiagnostics::json(uint64_t now, const std::vector<size_t>& depths, size_t net, bool full)
{
    return serializeQueueDiagnostics(collect(now, depths, net, full));
}
