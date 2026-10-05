#ifndef CLEARMOON_ZONE_SCENESERVER_SERVICE_AOITYPES_H
#define CLEARMOON_ZONE_SCENESERVER_SERVICE_AOITYPES_H

#include <cstdint>
#include <functional>
#include <vector>

using workerId = uint32_t;
/**
 *  -----------------------------------------------------------------------------------------------
 *                                  AOI相关定义
 *  AOI实现方案：暂定每一个逻辑线程都持有全场景AOI快照，每一tick就更新本地AOI快照并投递同步消息给其他逻辑线程
 *  -----------------------------------------------------------------------------------------------
 */
using Position = uint32_t;
using EntityId = uint64_t;            // 场景内唯一运行时标识（不是 playerId）




enum class EntityKind : uint8_t
{
    kPlayer  = 1,
    kMonster = 2,   // 预留：需先定义 owner 规则（§7.3）
    kNpc     = 3,   // 预留
    kDrop    = 4,   // 预留

    kInvalid    = 255
};


//AOI实例类
struct AoiEntity
{   
    EntityId   entityId   = 0;        //所有 AOI 结构的键
    EntityKind kind       = EntityKind::kPlayer;    //实例类型
    uint64_t   businessId = 0;        // 玩家 = playerId；怪物/NPC = 配置 id
    Position    x = 0, y = 0;
    int32_t dir = 0; // 有符号，与 Player 对齐，可表达越界为负
    uint32_t   cellIdx    = 0;        // 所在格（放在实体里，省一张 entity→cell 表）

    // ---- 三个世代，职责不同，不可混用 ----
    uint32_t entityGeneration = 0;    // 实体"进入场景"的世代：每次 ENTER 递增；LEAVE 后作废
    uint32_t actorEpoch       = 0;    // 沿用 Player::epoch：DB/Timer 回调生命周期
    uint64_t spatialVersion   = 0;    // 仅位置/朝向的空间版本（作用域见 §5.1.2）

    // ---- 副本元数据 ----
    uint32_t ownerWorker = 0;         // 权威 owner（事件来源；非路由表）
    uint64_t ownerSeq    = 0;         // 该实体最后变更在 owner 事件流中的序号（水位）
};


enum class AoiMsgKind : uint8_t
{
    kEntry = 1,
    kMove  = 2,
    kLeave = 3,
    kInvalid  = 255
};

struct AoiEvent
{
    AoiMsgKind    msgKind = AoiMsgKind::kInvalid;
    EntityKind entityKind = EntityKind::kInvalid;

    EntityId         eId  = 0;
    uint64_t   businessId = 0;

    Position target_x  = 0;
    Position target_y  = 0;
    int32_t target_dir = 0;
    uint32_t cellIdx   = 0;

    // ---- 三个世代，职责不同，不可混用 ----
    uint32_t entityGeneration = 0;
    uint32_t actorEpoch       = 0; 
    uint64_t spatialVersion   = 0; 

    workerId ownerThreadId = 0;
    uint64_t ownerSeq      = 0;
};

struct AoiOwnerSnapshot
{
    uint64_t seq;
    workerId ownerWokerId;
    std::vector<AoiEntity> ents;
};

struct AoiBatchEvent
{

};


struct SessionEntry
{

};

struct AoiPushSink
{

};


enum AoiStats : uint8_t
{

};

inline void updateEntity(AoiEntity& ent, const AoiEvent& ev, uint32_t cellIdx)
{
    ent.entityId   = ev.eId;
    ent.kind       = ev.entityKind;
    ent.businessId = ev.businessId;

    ent.x   = ev.target_x;
    ent.y   = ev.target_y;
    ent.dir = ev.target_dir;
    ent.cellIdx = cellIdx;

    ent.actorEpoch       = ev.actorEpoch;
    ent.spatialVersion   = ev.spatialVersion;
    ent.entityGeneration = ev.entityGeneration;

    ent.ownerWorker = ev.ownerThreadId;
    ent.ownerSeq    = ev.ownerSeq;
}


#endif