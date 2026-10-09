#ifndef CLEARMOON_SCENE_AOI_WIRE_H
#define CLEARMOON_SCENE_AOI_WIRE_H

#include "common/msg.h"
#include "service/aoi/aoiTypes.h"
#include <cstring>
#include <unordered_set>

// 进程内小端协议，完整解码后才交给 Service；不序列化 cellIdx
namespace AoiWire {
constexpr uint16_t kVersion = 1;
constexpr size_t kEventBytes = 62;
constexpr size_t kMaxSnapshotEntities = 65536;

inline bool ValidKind(EntityKind k) 
{
    return k == EntityKind::kPlayer || k == EntityKind::kMonster ||
           k == EntityKind::kNpc || k == EntityKind::kDrop;
}

inline void U64(std::string& s, uint64_t v) 
{
    for(unsigned i = 0; i < 8; ++i) 
        s.push_back(char((v >> (8 * i)) & 255));
}

struct Reader 
{
    const std::string& body;
    size_t pos = 0;
    bool read(unsigned n, uint64_t& v) 
    {
        if(n > body.size() - pos) 
            return false;
        v = 0;
        for(unsigned i = 0; i < n; ++i) 
            v |= uint64_t(uint8_t(body[pos++])) << (8 * i);
        return true;
    }
};

inline bool ValidEvent(const AoiEvent& e) 
{
    return e.eId != 0 && ValidKind(e.entityKind) &&
           (e.msgKind == AoiMsgKind::kEntry || e.msgKind == AoiMsgKind::kMove ||
            e.msgKind == AoiMsgKind::kLeave);
}

inline void WriteEvent(std::string& s, const AoiEvent& e) 
{
    PutU16(s, uint16_t(e.msgKind)); 
    PutU16(s, uint16_t(e.entityKind));
    U64(s, e.eId); 
    U64(s, e.businessId);

    PutU32(s, e.target_x); 
    PutU32(s, e.target_y); 
    PutU32(s, uint32_t(e.target_dir));
    PutU32(s, e.entityGeneration); 
    PutU32(s, e.actorEpoch);
    U64(s, e.spatialVersion); 
    PutU32(s, e.ownerThreadId);
    U64(s, e.ownerSeq);
}

inline bool ReadEvent(Reader& r, AoiEvent& e) 
{
    uint64_t v;
    if(!r.read(2,v)) 
        return false;

    if(v < 1 || v > 3) 
        return false;

    e.msgKind = AoiMsgKind(v);
    if(!r.read(2,v) || v < 1 || v > 4) 
        return false;

    e.entityKind = EntityKind(v);
    if(!r.read(8,e.eId) || !r.read(8,e.businessId)) 
        return false;

    if(!r.read(4,v)) 
        return false;

    e.target_x = uint32_t(v);
    if(!r.read(4,v)) 
        return false;

    e.target_y = uint32_t(v);
    if(!r.read(4,v)) 
        return false;

    const uint32_t dir = uint32_t(v);
    std::memcpy(&e.target_dir, &dir, sizeof(dir));
    if(!r.read(4,v)) 
        return false;

    e.entityGeneration = uint32_t(v);
    if(!r.read(4,v)) 
        return false;
    
    e.actorEpoch = uint32_t(v);
    if(!r.read(8,e.spatialVersion) || !r.read(4,v)) 
        return false;

    e.ownerThreadId = uint32_t(v);
    return r.read(8,e.ownerSeq) && ValidEvent(e);
}

inline bool EncodeEvent(const AoiEvent& e, std::string& out) 
{
    if(!ValidEvent(e)) 
        return false;

    std::string s; 
    PutU16(s,kVersion); 
    WriteEvent(s,e); 
    out.swap(s); 
    return true;
}

inline bool DecodeEvent(const std::string& s, AoiEvent& out) 
{
    Reader r{s}; 
    uint64_t version; 
    AoiEvent e;

    if(!r.read(2,version) || version != kVersion || !ReadEvent(r,e) || r.pos != s.size()) 
        return false;
    out = e; 
    return true;
}

inline uint16_t EventMethod(AoiMsgKind kind) 
{
    switch(kind) 
    {
    case AoiMsgKind::kEntry: return Method::AOI_ENTRY;
    case AoiMsgKind::kMove: return Method::AOI_MOVE;
    case AoiMsgKind::kLeave: return Method::AOI_LEAVE;
    default: return 0;
    }
}

inline AoiEvent SnapshotEvent(const AoiEntity& e) 
{
    AoiEvent ev;
    ev.msgKind = AoiMsgKind::kEntry; ev.entityKind = e.kind;
    ev.eId = e.entityId; ev.businessId = e.businessId;
    ev.target_x = e.x; ev.target_y = e.y; ev.target_dir = e.dir;
    ev.entityGeneration = e.entityGeneration; ev.actorEpoch = e.actorEpoch;
    ev.spatialVersion = e.spatialVersion; ev.ownerThreadId = e.ownerWorker; ev.ownerSeq = e.ownerSeq;
    return ev;
}

inline bool EncodeSnapshot(const AoiOwnerSnapshot& snap, std::string& out) 
{
    if(snap.ents.size() > kMaxSnapshotEntities) 
        return false;
    std::unordered_set<EntityId> seen;
    std::string s; 
    PutU16(s,kVersion); 
    PutU32(s,snap.ownerWokerId); U64(s,snap.seq);
    PutU32(s,uint32_t(snap.ents.size()));

    for(const auto& ent : snap.ents) 
    {
        auto e = SnapshotEvent(ent);
        if(!ValidEvent(e) || e.ownerThreadId != snap.ownerWokerId || e.ownerSeq > snap.seq ||
           !seen.insert(e.eId).second) 
           return false;
        WriteEvent(s,e);
    }
    out.swap(s); return true;
}

inline bool DecodeSnapshot(const std::string& s, AoiOwnerSnapshot& out) 
{
    Reader r{s}; uint64_t v; AoiOwnerSnapshot snap{};
    if(!r.read(2,v) || v != kVersion || !r.read(4,v)) 
        return false;

    snap.ownerWokerId = uint32_t(v);
    if(!r.read(8,snap.seq) || !r.read(4,v) || v > kMaxSnapshotEntities) 
        return false;

    const auto count = size_t(v);
    // 每个实体 60 字节（不含协议版本）。
    if(s.size() - r.pos != count * (kEventBytes - 2)) 
        return false;

    std::unordered_set<EntityId> seen;
    snap.ents.reserve(count);
    for(size_t i = 0; i < count; ++i) 
    {
        AoiEvent e;
        if(!ReadEvent(r,e) || e.msgKind != AoiMsgKind::kEntry || e.ownerThreadId != snap.ownerWokerId ||
           e.ownerSeq > snap.seq || !seen.insert(e.eId).second) 
           return false;
        AoiEntity ent; 
        updateEntity(ent,e,0); snap.ents.push_back(ent);
    }
    out = std::move(snap); return true;
}

inline std::string SnapshotRequest() 
{ 
    std::string s; PutU16(s,kVersion); return s; 
}

inline bool ValidSnapshotRequest(const std::string& s) 
{
    return s.size() == 2 && GetU16(s.data()) == kVersion;
}
}
#endif