#ifndef SCENE_DB_PLAYER_SNAPSHOT_DAO_H
#define SCENE_DB_PLAYER_SNAPSHOT_DAO_H

#include "db/dbValue.h"
#include "player/playerSaveSnapshot.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace PlayerSnapshotDao {
inline dbv::DbTask buildLoad(uint64_t playerId) 
{
    if (!playerId || playerId > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        throw std::invalid_argument("invalid player load identity");
    dbv::DbTask task; task.kind = 4; task.playerId = playerId;
    const auto pid = std::to_string(playerId);
    for (const auto& sql : {
        "SELECT hp,mp,gold,pos_x,pos_y,dir,scene_id,save_version FROM player WHERE id=? FOR UPDATE",
        "SELECT capacity,next_slot,container_version FROM player_container WHERE player_id=? AND container_kind=1",
        "SELECT item_id,slot,quantity FROM player_item_stack WHERE player_id=? AND container_kind=1 ORDER BY slot"}) 
    {
        dbv::SqlStep step; 
        step.kind = 1; 
        step.args = {sql,pid}; 
        task.steps.push_back(std::move(step));
    }
    return task;
}

// 拒绝不完整或不兼容的持久状态，不能用默认值覆盖
// 已有角色。仅在版本为 0 时允许缺少背包元数据行。
inline bool parseLoad(uint64_t playerId, const dbv::DbResult& result,
                      PlayerSaveSnapshot& output, std::string& error) 
{
    auto fail = [&](const char* text) { error=text; return false; };

    if (result.errcode || result.classification != dbv::ResultClass::kSuccess ||
        result.playerId != playerId || !result.data.IsArr() || result.data.a.size()!=3)
        return fail("invalid player load result");
    
    const auto& main=result.data.a[0];
    const auto& container=result.data.a[1]; const auto& items=result.data.a[2];

    if (!main.IsArr() || main.a.size()!=1 || !main.a[0].IsArr() || main.a[0].a.size()!=8 ||
        !container.IsArr() || container.a.size()>1 || !items.IsArr())
        return fail("player missing or malformed rows");
    
    const auto& row=main.a[0].a;
    for (const auto& v:row) 
        if (!v.IsInt()) return fail("noninteger player state");
    
    if (row[0].i<0 || row[0].i>65535 || row[1].i<0 || row[1].i>65535 ||
        row[2].i<0 || uint64_t(row[2].i)>UINT32_MAX || row[6].i<=0 || uint64_t(row[6].i)>UINT32_MAX || row[7].i<0)
        return fail("player state out of range");

    for (size_t i=3;i<6;++i)
        if(row[i].i<INT32_MIN || row[i].i>INT32_MAX) return fail("position out of range");
    
    PlayerSaveSnapshot snap;
    snap.playerId=playerId; 
    snap.hp=uint16_t(row[0].i); 
    snap.mp=uint16_t(row[1].i);
    snap.gold=uint32_t(row[2].i); 
    snap.x=int32_t(row[3].i); 
    snap.y=int32_t(row[4].i);
    snap.dir=int32_t(row[5].i); 
    snap.sceneId=uint32_t(row[6].i); 
    snap.saveVersion=uint64_t(row[7].i);

    if(container.a.empty()) 
    {
        if(snap.saveVersion || !items.a.empty()) 
            return fail("missing persisted bag metadata");
    } 
    else {
        if(!container.a[0].IsArr() || container.a[0].a.size()!=3) 
            return fail("malformed bag metadata");

        const auto& b=container.a[0].a;
        for(const auto& v:b) 
            if(!v.IsInt()) 
                return fail("noninteger bag metadata");

        if(b[0].i<=0 || b[0].i>65536 || b[1].i<0 || b[1].i>65536 || b[2].i<0 || uint64_t(b[2].i)!=snap.saveVersion)
            return fail("bag metadata/version mismatch");
        snap.bagCapacity=uint32_t(b[0].i); snap.bagNextSlot=uint32_t(b[1].i);
    }

    std::unordered_set<uint32_t> ids; std::unordered_set<uint16_t> slots;
    for(const auto& item:items.a) 
    {
        if(!item.IsArr() || item.a.size()!=3) 
            return fail("malformed item row");
        for(const auto& v:item.a) 
            if(!v.IsInt()) 
                return fail("noninteger item row");
        
        const auto id=item.a[0].i, slot=item.a[1].i, count=item.a[2].i;
        if(id<=0 || uint64_t(id)>UINT32_MAX || slot<0 || slot>=snap.bagNextSlot || slot>65535 ||
            count<=0 || count>65535 || !ids.insert(uint32_t(id)).second || !slots.insert(uint16_t(slot)).second)
            return fail("invalid or duplicate stack");
        snap.items.push_back({uint32_t(id),uint16_t(count),uint16_t(slot)});
    }
    if(snap.items.size()>snap.bagCapacity) 
        return fail("bag capacity exceeded");
    output=std::move(snap); error.clear(); 
        return true;
}

// 要求玩家行已存在，不隐式创建角色资料或账号。
// 只有覆盖玩家主状态与背包完整范围的快照才允许版本跳跃。
inline bool buildSave(const PlayerSaveSnapshot& snapshot, dbv::DbTask& output, std::string& error) 
{
    const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (!snapshot.playerId || snapshot.playerId > max || !snapshot.saveVersion ||
        snapshot.saveVersion > max || !snapshot.sceneId || !snapshot.bagCapacity ||
        snapshot.bagCapacity > 65536 || snapshot.bagNextSlot > 65536 ||
        snapshot.items.size() > snapshot.bagCapacity) 
    {
        error = "invalid player snapshot metadata"; 
        return false;
    }

    auto items = snapshot.items;
    std::unordered_set<uint32_t> ids;
    std::unordered_set<uint16_t> slots;
    for (const auto& item : items) 
        if (!item.itemId || !item.count || item.slot >= snapshot.bagNextSlot ||
        !ids.insert(item.itemId).second || !slots.insert(item.slot).second)
        {
            error = "invalid or duplicate item stack"; 
            return false;
        }
    
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });

    const auto pid = std::to_string(snapshot.playerId), version = std::to_string(snapshot.saveVersion);

    dbv::DbTask task; 
    task.kind = 5;
    task.playerId = snapshot.playerId; 
    task.saveVersion = snapshot.saveVersion;
    task.capturedRevision = snapshot.capturedRevision; 
    task.saveLifecycleToken = snapshot.saveLifecycleToken;

    dbv::SqlStep player;
    player.args = {"UPDATE player SET hp=?,mp=?,gold=?,pos_x=?,pos_y=?,dir=?,scene_id=?,save_version=?,updated_at=CURRENT_TIMESTAMP(3) WHERE id=? AND save_version<?",
        std::to_string(snapshot.hp), std::to_string(snapshot.mp), std::to_string(snapshot.gold),
        std::to_string(snapshot.x), std::to_string(snapshot.y), std::to_string(snapshot.dir),
        std::to_string(snapshot.sceneId), version, pid, version};
    
    player.expectedAffected = 1;
    task.steps.push_back(std::move(player));

    dbv::SqlStep container;
    container.args = {"INSERT INTO player_container(player_id,container_kind,capacity,next_slot,container_version) VALUES(?,1,?,?,?) ON DUPLICATE KEY UPDATE capacity=VALUES(capacity),next_slot=VALUES(next_slot),container_version=VALUES(container_version),updated_at=CURRENT_TIMESTAMP(3)",
        pid, std::to_string(snapshot.bagCapacity), std::to_string(snapshot.bagNextSlot), version};

    task.steps.push_back(std::move(container));

    dbv::SqlStep erase;
    erase.args = {"DELETE FROM player_item_stack WHERE player_id=? AND container_kind=1",pid};

    task.steps.push_back(std::move(erase));

    for (const auto& item : items) 
    {
        dbv::SqlStep insert;
        insert.args = {"INSERT INTO player_item_stack(player_id,container_kind,item_id,slot,quantity) VALUES(?,1,?,?,?)",
            pid, std::to_string(item.itemId), std::to_string(item.slot), std::to_string(item.count)};
        insert.expectedAffected = 1;
        task.steps.push_back(std::move(insert));
    }
    auto verify = [&](std::string sql, dbv::Val expected) {
        dbv::SqlStep step; 
        step.kind=1; 
        step.args={std::move(sql),pid};
        step.checkData=true; 
        step.expectedData=std::move(expected); 
        task.verification.push_back(std::move(step));
    };

    auto number = [](auto value) { return dbv::Val::Int(static_cast<int64_t>(value)); };

    verify("SELECT hp,mp,gold,pos_x,pos_y,dir,scene_id,save_version FROM player WHERE id=?",
        dbv::Val::Arr({dbv::Val::Arr({number(snapshot.hp),number(snapshot.mp),number(snapshot.gold),
        number(snapshot.x),number(snapshot.y),number(snapshot.dir),number(snapshot.sceneId),number(snapshot.saveVersion)})}));
    
    verify("SELECT capacity,next_slot,container_version FROM player_container WHERE player_id=? AND container_kind=1",
        dbv::Val::Arr({dbv::Val::Arr({number(snapshot.bagCapacity),number(snapshot.bagNextSlot),number(snapshot.saveVersion)})}));
    
    std::vector<dbv::Val> rows;
    for (const auto& item : items) 
        rows.push_back(dbv::Val::Arr({number(item.itemId),number(item.slot),number(item.count)}));

    verify("SELECT item_id,slot,quantity FROM player_item_stack WHERE player_id=? AND container_kind=1 ORDER BY slot",
        dbv::Val::Arr(std::move(rows)));
    
    output=std::move(task); 
    error.clear(); 
    return true;
}
} // namespace PlayerSnapshotDao
#endif