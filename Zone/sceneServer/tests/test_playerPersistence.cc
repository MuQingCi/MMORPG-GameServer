#include "test_util.h"
#include "player/playerPersistence.h"

namespace {
dbv::DbResult Saved(const Msg& message) {
    dbv::DbTask task;
    dbv::DbTask::Decode(message.body,task);
    dbv::DbResult result;
    result.playerId=task.playerId; result.saveVersion=task.saveVersion;
    result.capturedRevision=task.capturedRevision;
    result.saveLifecycleToken=task.saveLifecycleToken;
    return result;
}
}

TEST(PersistenceConfirmsOnlyCapturedRevisionAndSerializes)
{
    Player p; p.set_id(42); p.set_epoch(1); p.setPos(10,20,0);
    PlayerPersistence store; std::vector<Msg> sent;
    auto submit=[&](Msg m) { sent.push_back(std::move(m)); return true; };
    CHECK(store.request(p,false,submit));
    CHECK(p.dirty()); CHECK_EQ(sent.size(),1u);
    CHECK(sent[0].diagnosticSaveTask);
    p.setPos(11,20,0);
    CHECK(store.request(p,false,submit));
    CHECK_EQ(sent.size(),1u);
    CHECK(store.complete(sent[0].head.taskId,Saved(sent[0]),&p));
    CHECK(p.dirty()); CHECK_EQ(p.persistedRevision(),1u);
    CHECK(!store.complete(sent[0].head.taskId,Saved(sent[0]),&p));
    store.tick(submit);
    CHECK_EQ(sent.size(),2u);
    CHECK(store.complete(sent[1].head.taskId,Saved(sent[1]),&p));
    CHECK(!p.dirty()); CHECK_EQ(p.persistedVersion(),2u);
}

TEST(PersistenceRetainsBackpressureAndOfflineSnapshot)
{
    PlayerPersistence store; Player p; p.set_id(42); p.set_epoch(1); p.addGold(10);
    CHECK(store.request(p,false,[](Msg) { return false; }));
    CHECK(p.dirty()); CHECK(store.find(42)->queued.has_value());
    p.addGold(5);
    CHECK(store.request(p,true,[](Msg) { return false; }));
    CHECK(store.blocksLogin(42));
    CHECK_EQ(store.find(42)->queued->gold,15u);
    std::vector<Msg> sent;
    store.tick([&](Msg m) { sent.push_back(std::move(m)); return true; });
    CHECK_EQ(sent.size(),1u);
    CHECK(store.complete(sent[0].head.taskId,Saved(sent[0]),nullptr));
    CHECK(!store.blocksLogin(42)); CHECK(!store.find(42));
}

TEST(PersistenceUnknownFailureBlocksLaterSnapshots)
{
    PlayerPersistence store; Player p; p.set_id(42); p.set_epoch(1); p.addGold(1);
    std::vector<Msg> sent;
    auto submit=[&](Msg m) { sent.push_back(std::move(m)); return true; };
    CHECK(store.request(p,false,submit));
    auto unknown=Saved(sent[0]); unknown.errcode=1;
    unknown.classification=dbv::ResultClass::kOutcomeUnknown;
    CHECK(store.complete(sent[0].head.taskId,unknown,&p));
    p.addGold(2);
    CHECK(store.request(p,true,submit));
    store.tick(submit);
    CHECK_EQ(sent.size(),1u); CHECK(p.dirty());
    CHECK(store.find(42)->active.has_value());
    CHECK(store.find(42)->queued.has_value());
    CHECK(store.blocksLogin(42));
}

TEST(PersistenceRejectsMismatchedSuccess)
{
    PlayerPersistence store; Player p; p.set_id(42); p.addGold(1);
    Msg sent;
    CHECK(store.request(p,false,[&](Msg m) { sent=std::move(m); return true; }));
    auto bad=Saved(sent); ++bad.saveLifecycleToken;
    CHECK(!store.complete(sent.head.taskId,bad,&p));
    CHECK(p.dirty()); CHECK_EQ(p.persistedVersion(),0u);
    CHECK(store.find(42)->state==PlayerPersistence::State::Blocked);
}

TEST(PersistenceOfflineFinalSnapshotWaitsForActiveWrite)
{
    PlayerPersistence store; Player p; p.set_id(42); p.set_epoch(1); p.addGold(1);
    std::vector<Msg> sent;
    auto submit=[&](Msg m) { sent.push_back(std::move(m)); return true; };
    CHECK(store.request(p,false,submit));
    p.addGold(10);
    CHECK(store.request(p,true,submit));
    CHECK_EQ(sent.size(),1u);
    CHECK(store.complete(sent[0].head.taskId,Saved(sent[0]),nullptr));
    CHECK(store.blocksLogin(42));
    store.tick(submit);
    CHECK_EQ(sent.size(),2u);
    CHECK(sent[0].head.taskId!=sent[1].head.taskId);
    CHECK_EQ(Saved(sent[1]).saveVersion,2u);
    CHECK(store.complete(sent[1].head.taskId,Saved(sent[1]),nullptr));
    CHECK(!store.blocksLogin(42));
}

TEST(PersistenceClientRebindDoesNotDiscardSaveResult)
{
    PlayerPersistence store; Player p; p.set_id(42); p.set_epoch(1); p.addGold(1);
    std::vector<Msg> sent;
    auto submit=[&](Msg m) { sent.push_back(std::move(m)); return true; };
    CHECK(store.request(p,false,submit));
    p.set_epoch(2); // 同一个现存 Player，并非重新创建的 Actor。
    CHECK(store.request(p,false,submit));
    CHECK(store.complete(sent[0].head.taskId,Saved(sent[0]),&p));
    CHECK_EQ(p.persistedVersion(),1u);
    CHECK(!p.dirty());
}