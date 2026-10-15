#include "test_util.h"
#include "player/player.h"

TEST(EconomyReservationIsStrictAndPreservesLivePosition)
{
    Player player; player.set_id(42); player.addGold(100);
    const auto initial=player.mutationRevision();
    Player::EconomyRequest request{"purchase-1",30,7,2,1};
    CHECK(player.reserveEconomy(request));
    CHECK(player.economyPending());
    CHECK_EQ(player.gold(),100u); CHECK_EQ(player.bag().countOf(7),0u);
    CHECK_EQ(player.mutationRevision(),initial);
    const auto candidate=player.economyCandidate();
    CHECK(candidate); CHECK_EQ(candidate->gold,70u);
    CHECK_EQ(candidate->items.size(),1u); CHECK_EQ(candidate->items[0].count,2u);
    CHECK(player.reserveEconomy(request)); // 重复提交相同的待处理计划。
    auto conflict=request; ++conflict.cost;
    CHECK(!player.reserveEconomy(conflict));
    CHECK(!player.addItem(8,1)); CHECK(!player.consumeItem(7,1));
    bool blocked=false;
    try { player.addGold(1); } catch(const std::logic_error&) { blocked=true; }
    CHECK(blocked);
    player.setPos(11,20,0);
    CHECK(!player.commitEconomy("wrong-operation"));
    CHECK(player.commitEconomy(request.operationId));
    CHECK_EQ(player.gold(),70u); CHECK_EQ(player.bag().countOf(7),2u);
    CHECK_EQ(player.x(),11); CHECK_EQ(player.y(),20);
    CHECK_EQ(player.mutationRevision(),initial+2);
    CHECK(!player.economyPending());
    CHECK(!player.commitEconomy(request.operationId));
}

TEST(EconomyRejectionsAndCancellationLeaveStateUntouched)
{
    Player player; player.set_id(42); player.addGold(5);
    const auto revision=player.mutationRevision();
    CHECK(!player.reserveEconomy({"poor",6,7,1,1}));
    CHECK(!player.reserveEconomy({"",1,7,1,1}));
    CHECK(!player.reserveEconomy({"bad",1,0,1,1}));
    CHECK(!player.reserveEconomy({"bad",1,7,0,1}));
    CHECK(!player.reserveEconomy({"bad",1,7,1,0}));
    CHECK_EQ(player.gold(),5u); CHECK_EQ(player.mutationRevision(),revision);
    CHECK(player.reserveEconomy({"cancel",2,7,1,1}));
    CHECK(!player.cancelEconomy("wrong"));
    bool blocked=false;
    try { player.clearBag(); } catch(const std::logic_error&) { blocked=true; }
    CHECK(blocked);
    const auto snapshot=player.freezeSaveSnapshot(1,1);
    CHECK(!player.restoreSaveSnapshot(snapshot));
    CHECK(player.cancelEconomy("cancel"));
    CHECK_EQ(player.gold(),5u); CHECK_EQ(player.bag().used(),0u);
    CHECK_EQ(player.mutationRevision(),revision);
    CHECK(!player.cancelEconomy("cancel"));
    CHECK(player.addItem(7,65535));
    CHECK(!player.reserveEconomy({"overflow",1,7,1,1}));
    CHECK_EQ(player.gold(),5u); CHECK_EQ(player.bag().countOf(7),65535u);
}

TEST(EconomyCommitPreservesUnrelatedPersistentChanges)
{
    Player player; player.set_id(42); player.addGold(50);
    CHECK(player.reserveEconomy({"purchase",10,9,1,2}));
    player.modifyHp(-10); player.modifyMp(-5); player.setSceneId(2);
    CHECK(player.commitEconomy("purchase"));
    CHECK_EQ(player.hp(),90u); CHECK_EQ(player.mp(),45u); CHECK_EQ(player.sceneId(),2u);
    CHECK_EQ(player.gold(),40u); CHECK_EQ(player.bag().countOf(9),1u);
}