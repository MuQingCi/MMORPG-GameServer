#ifndef CLEARMOON_GATEWAY_TESTS_LOAD_CLIENT_PROTOCOL_H
#define CLEARMOON_GATEWAY_TESTS_LOAD_CLIENT_PROTOCOL_H

#include "base/clientProto.h"
#include "gameProto.pb.h"

#include <cstdint>
#include <string>

enum class LoadResponseKind
{
    Success,
    BusinessError,
    GatewayReject,
    Mismatch
};

inline bool MatchesLoadRequestResponse(const ClientHeader& header, uint64_t requestId,
                                      uint16_t module, uint16_t method)
{
    return (header.kind == ClientKind::kResponse || header.kind == ClientKind::kError) &&
           header.requestId == requestId && header.module == module && header.method == method;
}

inline bool IsRetTipBody(const std::string& body)
{
    gs::RetTip tip;
    gs::WalkAck walk;
    gs::EnterSceneAck enter;
    if (!tip.ParseFromString(body) || tip.code() == 0 || tip.text().empty()) return false;
    const bool walkValid = walk.ParseFromString(body) && walk.ByteSizeLong() == body.size();
    const bool enterValid = enter.ParseFromString(body) && enter.ByteSizeLong() == body.size();
    return !walkValid && !enterValid;
}

inline bool HasProtobufField(const std::string& body, uint32_t fieldNumber, uint8_t wireType)
{
    uint64_t tag = 0;
    for (uint8_t byte : body)
    {
        tag = (tag << 7) | (byte & 0x7f);
        if ((byte & 0x80) != 0) continue;
        if ((tag >> 3) == fieldNumber && (tag & 7) == wireType) return true;
        tag = 0;
    }
    return false;
}

inline LoadResponseKind ClassifyLoadResponse(const ClientHeader& header, const std::string& body,
                                             uint16_t expectedModule, uint16_t expectedMethod)
{
    if (header.module != expectedModule || header.method != expectedMethod)
        return LoadResponseKind::Mismatch;
    if (header.kind == ClientKind::kError)
        return expectedModule == kClientSysModule && expectedMethod == ClientSysMethod::kPing
            ? LoadResponseKind::GatewayReject : LoadResponseKind::BusinessError;
    if (header.kind != ClientKind::kResponse)
        return LoadResponseKind::Mismatch;
    if (expectedModule == 3 && expectedMethod == 2)
    {
        gs::RetTip tip;
        gs::WalkAck ack;
        const bool hasRetTipText = HasProtobufField(body, 2, 2);
        const bool hasWalkCoordinate = HasProtobufField(body, 2, 0) || HasProtobufField(body, 3, 0);
        if (hasRetTipText && !hasWalkCoordinate && IsRetTipBody(body))
            return tip.ParseFromString(body) && tip.code() == 0 ? LoadResponseKind::Success : LoadResponseKind::BusinessError;
        if (hasRetTipText == hasWalkCoordinate) return LoadResponseKind::Mismatch;
        if (!ack.ParseFromString(body) || ack.ByteSizeLong() != body.size() || body.empty()) return LoadResponseKind::Mismatch;
        return ack.code() == 0 ? LoadResponseKind::Success : LoadResponseKind::BusinessError;
    }
    if (expectedModule == 3 && expectedMethod == 4)
    {
        gs::RetTip tip;
        gs::EnterSceneAck ack;
        const bool hasRetTipText = HasProtobufField(body, 2, 2);
        const bool hasEnterPayload = HasProtobufField(body, 2, 0) || HasProtobufField(body, 3, 0) ||
                                     HasProtobufField(body, 4, 0) || HasProtobufField(body, 5, 0) ||
                                     HasProtobufField(body, 6, 0);
        if (hasRetTipText && !hasEnterPayload && IsRetTipBody(body))
            return tip.ParseFromString(body) && tip.code() == 0 ? LoadResponseKind::Success : LoadResponseKind::BusinessError;
        if (hasRetTipText == hasEnterPayload) return LoadResponseKind::Mismatch;
        if (!ack.ParseFromString(body) || ack.ByteSizeLong() != body.size() || body.empty()) return LoadResponseKind::Mismatch;
        return ack.code() == 0 ? LoadResponseKind::Success : LoadResponseKind::BusinessError;
    }
    return LoadResponseKind::Success;
}

inline bool AdvanceWalkPosition(const std::string& walkAck, int32_t& x, int32_t& y)
{
    gs::WalkAck ack;
    if (!ack.ParseFromString(walkAck) || ack.code() != 0) return false;
    x = ack.x();
    y = ack.y();
    return true;
}

inline bool AdvanceEnterScenePosition(const std::string& enterSceneAck, int32_t& x, int32_t& y)
{
    gs::EnterSceneAck ack;
    if (!ack.ParseFromString(enterSceneAck) || ack.code() != 0) return false;
    x = ack.x();
    y = ack.y();
    return true;
}

inline bool ApplySceneStatePush(const ClientHeader& header, const std::string& body,
                                int32_t& x, int32_t& y)
{
    if (header.kind != ClientKind::kPush || header.module != 3 || header.method != 1)
        return false;
    gs::SCPushState state;
    if (!state.ParseFromString(body)) return false;
    x = state.x();
    y = state.y();
    return true;
}

#endif