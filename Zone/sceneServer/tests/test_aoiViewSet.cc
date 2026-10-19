#include "test_util.h"
#include "service/aoi/aoiViewSet.h"

namespace {
SentEntityState State(uint32_t generation, uint64_t version)
{
    SentEntityState state;
    state.businessId = 100;
    state.entityGeneration = generation;
    state.spatialVersion = version;
    return state;
}
}

TEST(AoiViewSnapshotAllPartsRequiredAndFrozenBaseline)
{
    AoiViewSet views;
    CHECK(views.registryObserver(1, 10));
    auto send = views.prepareSend(1);
    CHECK(send.has_value());
    CHECK(views.markPending(1, 99, *send));
    ViewBaseline baseline{{2, State(3, 5)}};
    auto token = views.beginSnapshot(1, baseline, 2);
    CHECK(token.has_value());
    CHECK(!views.prepareSend(1).has_value());
    CHECK(!views.beginSnapshot(1, baseline, 2).has_value());
    baseline[2].spatialVersion = 100;
    CHECK(views.references(2, 3));
    CHECK(!views.references(2, 4));
    CHECK(views.references(99, 999));
    CHECK(!views.commitSnapshot(*token));
    CHECK(views.markSnapshotPartAccepted(*token, 0));
    CHECK(views.markSnapshotPartAccepted(*token, 0));
    CHECK(!views.markSnapshotPartAccepted(*token, 2));
    CHECK(!views.commitSnapshot(*token));
    CHECK(views.needResync(1));
    CHECK(views.markSnapshotPartAccepted(*token, 1));
    CHECK(views.commitSnapshot(*token));
    CHECK(!views.commitSnapshot(*token));
    CHECK(!views.needResync(1));
    CHECK(!views.hasPending(1));
    const auto* state = views.findSentState(1, 2);
    CHECK(state != nullptr);
    if(state) CHECK_EQ(state->spatialVersion, 5u);
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, 2u);
    CHECK(!views.references(99));
}

TEST(AoiViewBorrowedBaselineIsReadOnlyAndTracksCommittedState)
{
    AoiViewSet views;
    CHECK(views.borrowCommittedBaseline(1) == nullptr);
    CHECK(views.registryObserver(1,10));
    const auto* baseline=views.borrowCommittedBaseline(1);
    CHECK(baseline != nullptr);
    CHECK(baseline->empty());
    auto snapshot=views.beginSnapshot(1,ViewBaseline{{2,State(1,5)}},1);
    CHECK(snapshot.has_value());
    CHECK(views.markSnapshotPartAccepted(*snapshot,0));
    CHECK(views.commitSnapshot(*snapshot));
    baseline=views.borrowCommittedBaseline(1);
    CHECK_EQ(baseline->at(2).spatialVersion,5u);
    auto token=views.prepareSend(1);
    CHECK(token.has_value());
    CHECK(views.commitAccepted(1,*token,{{AoiMsgKind::kMove,2,State(1,6)}}));
    CHECK_EQ(views.borrowCommittedBaseline(1)->at(2).spatialVersion,6u);
    CHECK(views.resetObserver(1,10,11));
    CHECK(views.borrowCommittedBaseline(1)->empty());
    CHECK(views.removeObserver(1,11));
    CHECK(views.borrowCommittedBaseline(1) == nullptr);
}

TEST(AoiViewSnapshotRevisionSessionAndAbortGuards)
{
    AoiViewSet views;
    views.registryObserver(1, 10);
    ViewBaseline baseline{{2, State(1, 1)}};
    auto old = views.beginSnapshot(1, baseline, 1);
    CHECK(old.has_value());
    CHECK(!views.markNeedResync(1, 9));
    CHECK(!views.markNeedResync(1, 11));
    CHECK(views.markNeedResync(1, 10));
    CHECK(!views.markSnapshotPartAccepted(*old, 0));
    CHECK(!views.references(2));
    auto current = views.beginSnapshot(1, baseline, 1);
    CHECK(current.has_value());
    CHECK(current->snapshotId != old->snapshotId);
    auto forged = *current;
    ++forged.partCount;
    CHECK(!views.markSnapshotPartAccepted(forged, 0));
    CHECK(views.abortSnapshot(*current));
    CHECK(!views.abortSnapshot(*current));
    CHECK(views.needResync(1));
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, 1u);
    current = views.beginSnapshot(1, baseline, 1);
    CHECK(views.resetObserver(1, 10, 11));
    CHECK(!views.commitSnapshot(*current));
    CHECK(!views.references(2));
    current = views.beginSnapshot(1, baseline, 1);
    CHECK(views.removeObserver(1, 11));
    CHECK(!views.markSnapshotPartAccepted(*current, 0));
}

TEST(AoiViewReferencesAcrossObserversAndReplacement)
{
    AoiViewSet views;
    views.registryObserver(1, 1);
    views.registryObserver(2, 1);
    for(EntityId observer : {1u, 2u})
    {
        auto token = views.beginSnapshot(observer, {{50, State(7, 1)}}, 1);
        CHECK(token.has_value());
        CHECK(views.markSnapshotPartAccepted(*token, 0));
        CHECK(views.commitSnapshot(*token));
    }
    CHECK(views.references(50));
    CHECK(views.references(50, 7));
    CHECK(!views.references(50, 8));
    CHECK(views.markNeedResync(1, 1));
    auto token = views.beginSnapshot(1, {}, 1);
    CHECK(token.has_value());
    CHECK(views.references(50, 7)); // 未完成快照不能提前释放旧基线。
    CHECK(views.markSnapshotPartAccepted(*token, 0));
    CHECK(views.commitSnapshot(*token));
    CHECK(views.references(50, 7)); // 第二个观察者仍引用。
    CHECK(views.removeObserver(2, 1));
    CHECK(!views.references(50));
    CHECK(views.markNeedResync(1, 1));
    CHECK(!views.beginSnapshot(1, {}, 0).has_value());
    CHECK(!views.beginSnapshot(99, {}, 1).has_value());
    CHECK(!views.beginSnapshot(1, {{0, State(1, 1)}}, 1).has_value());
}

TEST(AoiViewIncrementEntryMoveLeaveAndSequence)
{
    AoiViewSet views;
    views.registryObserver(1, 10);
    auto token = *views.prepareSend(1);
    CHECK(views.markPending(1, 20, token));
    CHECK(views.markPending(1, 21, token));
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, State(1, 5)}}));
    CHECK_EQ(views.findSentState(1, 20)->spatialVersion, 5u);
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, 2u);
    CHECK(!views.findObserver(1)->pendings_.contains(20));
    CHECK(views.findObserver(1)->pendings_.contains(21));
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kMove, 20, State(1, 6)}}));
    token = *views.prepareSend(1);
    CHECK(views.commitAccepted(1, token, {}));
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, token.nextViewSeq);
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kMove, 20, State(1, 6)}}));
    CHECK_EQ(views.findSentState(1, 20)->spatialVersion, 6u);
    token = *views.prepareSend(1);
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kLeave, 20, State(1, 6)}}));
    CHECK(views.findSentState(1, 20) == nullptr);
    CHECK(!views.references(20));
    token = *views.prepareSend(1);
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kLeave, 20, State(1, 6)}}));
    CHECK(views.findSentState(1, 20) == nullptr);
}

TEST(AoiViewIncrementAtomicFailureAndIdentity)
{
    AoiViewSet views;
    views.registryObserver(1, 10);
    auto token = *views.prepareSend(1);
    views.markPending(1, 20, token);
    CHECK(!views.commitAccepted(1, token, {
        {AoiMsgKind::kEntry, 20, State(1, 5)},
        {AoiMsgKind::kMove, 99, State(1, 1)}}));
    CHECK(views.findSentState(1, 20) == nullptr);
    CHECK(views.findObserver(1)->pendings_.contains(20));
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, 1u);
    CHECK(!views.commitAccepted(1, token, {{static_cast<AoiMsgKind>(42), 20, State(1, 5)}}));
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 0, State(1, 5)}}));
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, State(1, 5)}}));
    token = *views.prepareSend(1);
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kMove, 20, State(1, 4)}}));
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kMove, 20, State(2, 1)}}));
    auto wrong = State(1, 6);
    wrong.businessId = 999;
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kMove, 20, wrong}}));
    wrong = State(1, 6);
    wrong.kind = EntityKind::kNpc;
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, wrong}}));
    CHECK_EQ(views.findSentState(1, 20)->spatialVersion, 5u);
    CHECK_EQ(views.prepareSend(1)->nextViewSeq, token.nextViewSeq);
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, State(2, 1)}}));
    token = *views.prepareSend(1);
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kLeave, 20, State(1, 100)}}));
    CHECK_EQ(views.findSentState(1, 20)->entityGeneration, 2u);
}

TEST(AoiViewIncrementOrderedReplacementAndTokenGuards)
{
    AoiViewSet views;
    views.registryObserver(1, 10);
    views.registryObserver(2, 10);
    auto token = *views.prepareSend(1);
    CHECK(!views.commitAccepted(2, token, {{AoiMsgKind::kEntry, 20, State(1, 1)}}));
    CHECK(views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, State(1, 10)}}));
    token = *views.prepareSend(1);
    CHECK(views.commitAccepted(1, token, {
        {AoiMsgKind::kLeave, 20, State(1, 10)},
        {AoiMsgKind::kEntry, 20, State(2, 1)},
        {AoiMsgKind::kMove, 20, State(2, 2)}}));
    CHECK_EQ(views.findSentState(1, 20)->entityGeneration, 2u);
    CHECK_EQ(views.findSentState(1, 20)->spatialVersion, 2u);
    token = *views.prepareSend(1);
    auto snapshot = views.beginSnapshot(1, {}, 1);
    CHECK(snapshot.has_value());
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kLeave, 20, State(2, 2)}}));
    CHECK(views.abortSnapshot(*snapshot));
    CHECK(views.resetObserver(1, 10, 11));
    CHECK(!views.commitAccepted(1, token, {{AoiMsgKind::kEntry, 20, State(2, 2)}}));
    CHECK(views.findSentState(1, 20) == nullptr);
}