#include "loadClientProtocol.h"
#include "test_util.h"

#include "gameProto.pb.h"

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