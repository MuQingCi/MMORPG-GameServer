#include "test_util.h"
#include "service/aoi/aoiReplica.h"
#include <algorithm>
#include <stdexcept>

namespace {
using Result = AoiReplica::ApplyResult;
using Source = AoiReplica::Source;
struct Fixture {
    Map map;
    AoiConfig config;
    std::unordered_set<uint64_t> dirty;
    Fixture() {
        std::string err;
        if (!Map::Load(std::string(SCENE_SOURCE_DIR) + "/config/map.yaml", 50, map, err))
            throw std::runtime_error(err);
    }
};
AoiEvent Event(EntityId id, workerId owner, uint64_t seq, uint32_t gen = 1,
               uint64_t version = 1, uint32_t x = 10) {
    AoiEvent ev;
    ev.msgKind = AoiMsgKind::kEntry;
    ev.entityKind = EntityKind::kPlayer;
    ev.eId = id;
    ev.businessId = id;
    ev.ownerThreadId = owner;
    ev.ownerSeq = seq;
    ev.entityGeneration = gen;
    ev.spatialVersion = version;
    ev.target_x = x;
    ev.target_y = 10;
    return ev;
}
AoiEntity Entity(const AoiEvent& ev) {
    AoiEntity ent;
    updateEntity(ent, ev, 999); // 接收方必须重新计算格子。
    return ent;
}
bool InCell(const AoiReplica& replica, uint32_t cell, EntityId id) {
    const auto& ids = replica.cell(cell).entys_;
    return std::count(ids.begin(), ids.end(), id) == 1;
}
}

TEST(AoiReplicaUnifiedStampAndUnknownMove) {
    Fixture f;
    AoiReplica replica(0, f.config, f.map);
    auto ev = Event(1, 0, 1);
    ev.msgKind = AoiMsgKind::kMove;
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kNeedSnapshot);
    ev.msgKind = AoiMsgKind::kEntry;
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kApplied);
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kIgnored);
    ev.msgKind = AoiMsgKind::kMove;
    ev.entityGeneration = 2;
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kNeedSnapshot);
    ev.msgKind = AoiMsgKind::kEntry;
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kApplied);
    ev.msgKind = AoiMsgKind::kLeave;
    ev.entityGeneration = 1;
    ev.spatialVersion = 100;
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kIgnored);
    CHECK(replica.findById(1) != nullptr);
    ev = Event(2, 1, 2);
    CHECK_EQ(replica.Apply(ev, Source::kLocal, f.dirty), Result::kBadSource);
    ev.msgKind = AoiMsgKind::kMove;
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kNeedSnapshot);
}

TEST(AoiReplicaSnapshotAlignsGridAndMissingEntities) {
    Fixture f;
    AoiReplica replica(0, f.config, f.map);
    CHECK_EQ(replica.Apply(Event(1, 1, 1), Source::kRemote, f.dirty), Result::kApplied);
    CHECK_EQ(replica.Apply(Event(2, 1, 2), Source::kRemote, f.dirty), Result::kApplied);
    CHECK_EQ(replica.Apply(Event(3, 2, 1), Source::kRemote, f.dirty), Result::kApplied);
    replica.addWatcher(90, 0);
    replica.addWatcher(91, 2);
    f.dirty.clear();
    AoiOwnerSnapshot snap{10, 1, {Entity(Event(1, 1, 5, 1, 2, 110)), Entity(Event(4, 1, 6))}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kApplied);
    CHECK(InCell(replica, 2, 1));
    CHECK(!InCell(replica, 0, 1));
    CHECK(InCell(replica, 0, 4));
    CHECK(replica.findById(2) == nullptr);
    CHECK(replica.findById(3) != nullptr);
    CHECK(f.dirty.count(90) != 0 && f.dirty.count(91) != 0);
    CHECK_EQ(replica.Apply(Event(2, 1, 9), Source::kRemote, f.dirty), Result::kIgnored);
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kIgnored);
}

TEST(AoiReplicaSnapshotPreservesPostWatermarkChanges) {
    Fixture f;
    AoiReplica replica(0, f.config, f.map);
    replica.Apply(Event(1, 1, 20, 1, 3, 110), Source::kRemote, f.dirty);
    auto leave = Event(2, 1, 21, 1, 4);
    leave.msgKind = AoiMsgKind::kLeave;
    CHECK_EQ(replica.Apply(leave, Source::kRemote, f.dirty), Result::kApplied);
    AoiOwnerSnapshot snap{10, 1, {Entity(Event(1, 1, 5)), Entity(Event(2, 1, 6))}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kApplied);
    CHECK_EQ(replica.findById(1)->x, 110u);
    CHECK(InCell(replica, 2, 1));
    CHECK(replica.findById(2) == nullptr);
    AoiOwnerSnapshot empty{11, 1, {}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(empty, f.dirty), Result::kApplied);
    CHECK(replica.findById(1) != nullptr);
}

TEST(AoiReplicaInvalidSnapshotDoesNotAdvanceWatermark) {
    Fixture f;
    AoiReplica replica(0, f.config, f.map);
    AoiOwnerSnapshot snap{10, 1, {Entity(Event(1, 1, 1)), Entity(Event(1, 1, 2))}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kInvalid);
    CHECK(replica.findById(1) == nullptr);
    CHECK(f.dirty.empty());
    CHECK_EQ(replica.Apply(Event(2, 1, 3), Source::kRemote, f.dirty), Result::kApplied);
    snap.ents = {Entity(Event(1, 1, 4, 1, 1, 2000))};
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kOutOfBounds);
    snap.ents.clear();
    CHECK_EQ(replica.ApplyOwnerSnapshot(snap, f.dirty), Result::kApplied);
    CHECK(replica.findById(2) == nullptr);
}

TEST(AoiReplicaLeaveAndIdentityGuards) {
    Fixture f;
    AoiReplica replica(0, f.config, f.map);
    auto ev = Event(1, 1, 1);
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kApplied);
    ev.msgKind = AoiMsgKind::kMove;
    ev.ownerSeq = 2;
    ev.spatialVersion = 2;
    ev.businessId = 99;
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kInvalid);
    ev.businessId = 1;
    ev.ownerThreadId = 2;
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kBadSource);
    ev.ownerThreadId = 1;
    ev.msgKind = AoiMsgKind::kLeave;
    ev.target_x = 999999; // 删除不要求目标坐标有效。
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kApplied);
    CHECK(replica.findById(1) == nullptr);
    CHECK(!InCell(replica, 0, 1));
    AoiOwnerSnapshot old{1, 1, {Entity(Event(1, 1, 1))}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(old, f.dirty), Result::kApplied);
    CHECK(replica.findById(1) == nullptr);
    ev = Event(1, 1, 3, 1, 3);
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kIgnored);
    ev.entityGeneration = 2;
    ev.spatialVersion = 1;
    CHECK_EQ(replica.Apply(ev, Source::kRemote, f.dirty), Result::kApplied);
    AoiOwnerSnapshot conflict{4, 2, {Entity(Event(1, 2, 4))}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(conflict, f.dirty), Result::kBadSource);
    AoiOwnerSnapshot self{4, 0, {}};
    CHECK_EQ(replica.ApplyOwnerSnapshot(self, f.dirty), Result::kBadSource);
}