#include "test_util.h"
#include "player/player.h"
#include <limits>

TEST(PlayerRevisionTracksOnlyRealMutations)
{
    Player p;
    p.set_id(42);
    p.set_hp(100); p.set_mp(50); p.setPos(0,0,0);
    p.modifyHp(0); p.modifyMp(0); p.addGold(-1);
    CHECK_EQ(p.mutationRevision(),0u);
    CHECK(!p.dirty());
    p.modifyHp(-5);
    CHECK_EQ(p.mutationRevision(),1u);
    p.set_mp(60);
    p.addGold(std::numeric_limits<int64_t>::max());
    CHECK_EQ(p.gold(),std::numeric_limits<uint32_t>::max());
    p.addGold(1);
    CHECK_EQ(p.mutationRevision(),3u);
    p.addGold(std::numeric_limits<int64_t>::min());
    CHECK_EQ(p.gold(),0u);
    p.setPos(10,20,3);
    CHECK_EQ(p.mutationRevision(),5u);
    p.clearDirty();
    CHECK_EQ(p.mutationRevision(),5u);
    p.setPos(10,20,3);
    CHECK(!p.dirty());
    p.setSceneId(2);
    CHECK_EQ(p.mutationRevision(),6u);
    p.setSceneId(2);
    CHECK_EQ(p.mutationRevision(),6u);
}

TEST(PlayerBagMutationsAndFixedSnapshot)
{
    Player p; p.set_id(42);
    CHECK(!p.addItem(0,1)); CHECK(!p.consumeItem(1,1));
    p.clearBag();
    CHECK_EQ(p.mutationRevision(),0u);
    CHECK(p.addItem(1,5));
    CHECK_EQ(p.mutationRevision(),1u);
    CHECK(!p.addItem(1,65535));
    CHECK_EQ(p.mutationRevision(),1u);
    p.setPos(10,20,1); p.setSceneId(2);
    const auto snapshot = p.freezeSaveSnapshot(101,8);
    CHECK_EQ(snapshot.saveVersion,101u);
    CHECK_EQ(snapshot.saveLifecycleToken,8u);
    CHECK_EQ(snapshot.capturedRevision,3u);
    CHECK_EQ(snapshot.bagNextSlot,1u);
    CHECK_EQ(snapshot.items[0].count,5u);
    CHECK(p.consumeItem(1,2));
    p.setPos(30,40,2);
    CHECK_EQ(snapshot.items[0].count,5u);
    CHECK_EQ(snapshot.x,10);
    CHECK_EQ(snapshot.sceneId,2u);
    p.clearBag();
    CHECK_EQ(p.mutationRevision(),6u);
    p.clearBag();
    CHECK_EQ(p.mutationRevision(),6u);
}

TEST(PlayerSnapshotRestoreIsValidatedAndNotBusinessMutation)
{
    Player source; source.set_id(42); source.setPos(10,20,1);
    CHECK(source.addItem(9,4));
    auto snapshot = source.freezeSaveSnapshot(7,12);
    Player loaded; loaded.set_session(99); loaded.set_epoch(3);
    CHECK(loaded.restoreSaveSnapshot(snapshot));
    CHECK_EQ(loaded.mutationRevision(),0u);
    CHECK(!loaded.dirty());
    CHECK_EQ(loaded.session(),99u); CHECK_EQ(loaded.epoch(),3u);
    CHECK_EQ(loaded.bag().countOf(9),4u);
    snapshot.items.push_back(snapshot.items[0]);
    CHECK(!loaded.restoreSaveSnapshot(snapshot));
    CHECK_EQ(loaded.bag().used(),1u); CHECK_EQ(loaded.x(),10);
    snapshot.items.pop_back(); snapshot.playerId=43;
    CHECK(!loaded.restoreSaveSnapshot(snapshot));
    CHECK_EQ(loaded.id(),42u);
}

TEST(BagSlotExhaustionNeverWraps)
{
    Bag bag;
    CHECK(bag.restore(64,65535,{}));
    CHECK(bag.addItem(1,1));
    CHECK_EQ(bag.view()[0].slot,65535u);
    CHECK_EQ(bag.nextSlot(),65536u);
    CHECK(!bag.addItem(2,1));
    CHECK(bag.addItem(1,1)); // 已有堆叠仍可使用。
    CHECK(bag.reduceItem(1,2));
    CHECK(!bag.addItem(2,1)); // 删除堆叠不会回收槽位编号。
    CHECK(!bag.restore(64,65537,{}));
    CHECK_EQ(bag.nextSlot(),65536u);
    bag.clear();
    CHECK_EQ(bag.nextSlot(),0u);
}