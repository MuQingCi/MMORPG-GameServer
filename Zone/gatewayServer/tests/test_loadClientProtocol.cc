#include "loadClientProtocol.h"
#include "test_util.h"

#include "gameProto.pb.h"

TEST(LoadClientProtocolSkipsCoordinateValuesWhenReadingFieldTags)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 2;
    for (int32_t coordinate : {18, 26, 146, 0, -1})
    {
        gs::WalkAck ack;
        ack.set_x(coordinate);
        ack.set_y(20);
        CHECK(!HasProtobufField(ack.SerializeAsString(), 2, 2));
        CHECK_EQ(ClassifyLoadResponse(header, ack.SerializeAsString(), 3, 2),
                 LoadResponseKind::Success);
    }
    gs::RetTip tip;
    tip.set_code(1002);
    tip.set_text("position mismatch");
    CHECK(IsRetTipBody(tip.SerializeAsString()));
    CHECK(!HasProtobufField(tip.SerializeAsString(), 2, 0));
}

TEST(LoadClientProtocolRejectsBusinessCodeError)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 2;
    header.requestId = 17;
    gs::WalkAck ack;
    ack.set_code(1002);
    ack.set_x(10);
    ack.set_y(20);
    CHECK_EQ(ClassifyLoadResponse(header, ack.SerializeAsString(), 3, 2),
             LoadResponseKind::BusinessError);
}

TEST(LoadClientProtocolRejectsRetTipErrorResponse)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 2;
    gs::RetTip tip;
    tip.set_code(1002);
    tip.set_text("position mismatch");
    CHECK_EQ(ClassifyLoadResponse(header, tip.SerializeAsString(), 3, 2),
             LoadResponseKind::BusinessError);
}

TEST(LoadClientProtocolDoesNotClassifyControlRetTipAsBusinessResponse)
{
    ClientHeader header;
    header.kind = ClientKind::kControl;
    header.module = 3;
    header.method = 2;
    gs::RetTip tip;
    tip.set_code(1002);
    tip.set_text("position mismatch");
    CHECK_EQ(ClassifyLoadResponse(header, tip.SerializeAsString(), 3, 2),
             LoadResponseKind::Mismatch);
    header.kind = ClientKind::kResponse;
    CHECK_EQ(ClassifyLoadResponse(header, tip.SerializeAsString(), 3, 2),
             LoadResponseKind::BusinessError);
}

TEST(LoadClientProtocolClassifiesAmbiguousProtobufBodiesAsMismatch)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 2;
    gs::RetTip ambiguous;
    ambiguous.set_code(1002);
    CHECK_EQ(ClassifyLoadResponse(header, ambiguous.SerializeAsString(), 3, 2),
             LoadResponseKind::Mismatch);
}

TEST(LoadClientProtocolRequiresExpectedResponseTuple)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 4;
    header.requestId = 17;
    CHECK_EQ(ClassifyLoadResponse(header, "", 3, 2), LoadResponseKind::Mismatch);
}

TEST(LoadClientProtocolDoesNotPairAsynchronousPushWithRequest)
{
    ClientHeader header;
    header.kind = ClientKind::kPush;
    header.module = 100;
    header.method = 8;
    header.requestId = 0;
    CHECK(!MatchesLoadRequestResponse(header, 2, 3, 4));
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 4;
    header.requestId = 2;
    CHECK(MatchesLoadRequestResponse(header, 2, 3, 4));
}

TEST(LoadClientProtocolClassifiesBusinessErrorSeparatelyFromGatewayReject)
{
    ClientHeader header;
    header.kind = ClientKind::kError;
    header.module = 3;
    header.method = 2;
    CHECK_EQ(ClassifyLoadResponse(header, "", 3, 2), LoadResponseKind::BusinessError);
    header.module = kClientSysModule;
    header.method = ClientSysMethod::kPing;
    CHECK_EQ(ClassifyLoadResponse(header, "", kClientSysModule, ClientSysMethod::kPing),
             LoadResponseKind::GatewayReject);
}

TEST(LoadClientProtocolAdvancesOnlyFromSuccessfulWalkAck)
{
    gs::WalkAck ack;
    ack.set_code(0);
    ack.set_x(11);
    ack.set_y(20);
    int32_t x = 10, y = 20;
    CHECK(AdvanceWalkPosition(ack.SerializeAsString(), x, y));
    CHECK_EQ(x, 11);
    CHECK_EQ(y, 20);

    ack.set_code(1002);
    ack.set_x(12);
    CHECK(!AdvanceWalkPosition(ack.SerializeAsString(), x, y));
    CHECK_EQ(x, 11);
    CHECK_EQ(y, 20);
}

TEST(LoadClientProtocolAppliesAuthoritativeSceneStatePush)
{
    ClientHeader header;
    header.kind = ClientKind::kPush;
    header.module = 3;
    header.method = 1;
    gs::SCPushState push;
    push.set_x(14);
    push.set_y(22);
    int32_t x = 10, y = 20;
    CHECK(ApplySceneStatePush(header, push.SerializeAsString(), x, y));
    CHECK_EQ(x, 14);
    CHECK_EQ(y, 22);
    header.method = 2;
    CHECK(!ApplySceneStatePush(header, push.SerializeAsString(), x, y));
}

TEST(LoadClientProtocolUsesEnterScenePositionAsMovementOrigin)
{
    gs::EnterSceneAck ack;
    ack.set_code(0);
    ack.set_x(10);
    ack.set_y(20);
    int32_t x = 0, y = 0;
    CHECK(AdvanceEnterScenePosition(ack.SerializeAsString(), x, y));
    CHECK_EQ(x, 10);
    CHECK_EQ(y, 20);
}

TEST(LoadClientProtocolRequiresMatchingSuccessfulLogoutAck)
{
    ClientHeader header;
    header.kind = ClientKind::kResponse;
    header.module = 3;
    header.method = 5;
    header.requestId = 91;
    gs::LogoutAck ack;
    CHECK(IsSuccessfulLogoutResponse(header, 91, ack.SerializeAsString()));
    CHECK(!IsSuccessfulLogoutResponse(header, 92, ack.SerializeAsString()));
    header.kind = ClientKind::kPush;
    CHECK(!IsSuccessfulLogoutResponse(header, 91, ack.SerializeAsString()));
    header.kind = ClientKind::kResponse;
    ack.set_code(1008);
    CHECK(!IsSuccessfulLogoutResponse(header, 91, ack.SerializeAsString()));
}
TEST(LoadWalkBouncesForLongRunsWithoutLeavingStrip)
{
    int32_t x = 517;
    int direction = 1;
    for (int i = 0; i < 10000; ++i)
    {
        const int32_t next = NextLoadWalkX(x, 517, direction);
        CHECK(next >= 517 && next <= 549);
        CHECK(next == x + 1 || next == x - 1);
        x = next;
    }
}

TEST(LoadWalkDoesNotReturnToAmbiguousZeroOrigin)
{
    int32_t x = 0;
    int direction = 1;
    for (int i = 0; i < 10000; ++i)
    {
        x = NextLoadWalkX(x, 0, direction);
        CHECK(x >= 1 && x <= 32);
    }
}
