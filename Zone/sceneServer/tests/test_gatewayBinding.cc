#include "test_util.h"
#include "base/gatewayPlayer.h"

TEST(GatewayEnvelopeRoundTripAndRejectsTruncation)
{
    ClientData d{100,200,3,4,5};
    const auto body=GatewayPlayer::Pack(d,"payload");
    ClientData out; std::string payload;
    CHECK(GatewayPlayer::Unpack(body,out,payload));
    CHECK(SameClientBinding(d,out));
    CHECK_EQ(payload,std::string("payload"));
    for(size_t n=0;n<GatewayPlayer::kHeaderSize;++n)
        CHECK(!GatewayPlayer::Unpack(body.substr(0,n),out,payload));
}

TEST(GatewayConnectionTracksMultiplePlayersAndOldOffline)
{
    GatewayClientIndex index;
    ClientData a{100,1,1,8,9}, b{200,2,1,8,9};
    CHECK(index.Bind(10,a)); CHECK(index.Bind(10,b));
    CHECK(index.Matches(10,a)); CHECK(index.Matches(10,b));
    auto newer=a; newer.clientEpoch=2; newer.clientSession=3;
    CHECK(index.Bind(20,newer));
    CHECK(!index.Unbind(10,a));
    const auto closed=index.Close(10);
    CHECK_EQ(closed.size(),1u); CHECK_EQ(closed[0].playerId,200u);
    CHECK(index.Matches(20,newer));
    CHECK(index.Unbind(20,newer));
    CHECK(!index.Bind(20,a));
    CHECK(!index.Bind(20,newer)); // 已结束世代不能因重复上线复活。
    newer.clientEpoch=3; newer.clientSession=4;
    CHECK(index.Bind(20,newer));
    auto otherGateway=newer; otherGateway.gatewayId=99; otherGateway.clientEpoch=100;
    CHECK(!index.Bind(30,otherGateway));
}