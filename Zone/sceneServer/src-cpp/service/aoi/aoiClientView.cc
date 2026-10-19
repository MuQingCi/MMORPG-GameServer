#include "service/aoi/aoiClientView.h"

#include "gameProto.pb.h"
#include "common/msg.h"
#include "service/aoi/aoiViewSet.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
using EntityView = std::vector<AoiEntity>;

gs::ViewEntity ToProto(const AoiEntity& entity)
{
    gs::ViewEntity out;
    out.set_entity_id(entity.entityId);
    out.set_business_id(entity.businessId);
    out.set_entity_kind(static_cast<uint32_t>(entity.kind));
    out.set_entity_generation(entity.entityGeneration);
    out.set_spatial_version(entity.spatialVersion);
    out.set_actor_epoch(entity.actorEpoch);
    out.set_x(static_cast<int32_t>(entity.x));
    out.set_y(static_cast<int32_t>(entity.y));
    out.set_dir(entity.dir);
    return out;
}

void FillProto(const AoiEntity& entity, gs::ViewEntity* out)
{
    *out = ToProto(entity);
}

EntityView CurrentView(AoiService& service, EntityId observer)
{
    std::vector<AoiEntity> entities;
    service.ViewOf(observer, entities);
    AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::BuildDiff);
    std::sort(entities.begin(), entities.end(), [](const auto& a, const auto& b) {
        return a.entityId < b.entityId;
    });
    return entities;
}

ViewBaseline ToBaseline(const EntityView& entities)
{
    ViewBaseline result;
    result.reserve(entities.size());
    for (const auto& entity : entities)
    {
        SentEntityState state;
        state.kind = entity.kind;
        state.businessId = entity.businessId;
        state.entityGeneration = entity.entityGeneration;
        state.spatialVersion = entity.spatialVersion;
        state.x = static_cast<int32_t>(entity.x);
        state.y = static_cast<int32_t>(entity.y);
        state.dir = entity.dir;
        result.emplace(entity.entityId, state);
    }
    return result;
}

bool SendDelta(AoiService& service, EntityId observer, const ViewSendToken& token,
               const EntityView& current, const ViewBaseline& baseline)
{
    gs::SCPushViewDelta delta;
    delta.set_view_seq(token.nextViewSeq);
    std::vector<AcceptedPush> accepted;
    {
    AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::BuildDiff);
    for (const auto& entity : current)
    {
        const auto id = entity.entityId;
        const auto old = baseline.find(id);
        if (old == baseline.end() || old->second.entityGeneration != entity.entityGeneration)
        {
            FillProto(entity, delta.add_entered());
            accepted.push_back({AoiMsgKind::kEntry, id,
                                {entity.kind, entity.businessId, entity.entityGeneration,
                                 entity.spatialVersion}});
        }
        else if (old->second.spatialVersion != entity.spatialVersion)
        {
            FillProto(entity, delta.add_moved());
            accepted.push_back({AoiMsgKind::kMove, id,
                                {entity.kind, entity.businessId, entity.entityGeneration,
                                 entity.spatialVersion}});
        }
    }

    for (const auto& [id, old] : baseline)
    {
        const auto found = std::lower_bound(current.begin(), current.end(), id,
            [](const AoiEntity& entity, EntityId key) { return entity.entityId < key; });
        if (found == current.end() || found->entityId != id)
        {
            auto* left = delta.add_left();
            left->set_entity_id(id);
            left->set_business_id(old.businessId);
            left->set_entity_kind(static_cast<uint32_t>(old.kind));
            left->set_entity_generation(old.entityGeneration);
            left->set_spatial_version(old.spatialVersion);
            left->set_x(old.x);
            left->set_y(old.y);
            left->set_dir(old.dir);
            accepted.push_back({AoiMsgKind::kLeave, id, old});
        }
    }
    }
    if (service.diagnostics()) 
        service.diagnostics()->add(AoiDiagnostics::Counter::PushEntities, accepted.size());

    if (accepted.empty())
        return true;

    AoiClientPushRequest request;
    request.observerPlayerId = 0;
    request.clientViewEpoch = token.clientViewEpoch;
    request.module = Module::AOI;
    request.method = Method::AOI_VIEW_DELTA;
    {
        AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::Serialize);
        request.body = delta.SerializeAsString();
    }

    // AoiPushSink 以 playerId 定位观察者；AOI 实体的 businessId 对玩家就是 playerId。
    const auto observerPlayerId = service.BusinessIdOf(observer);
    if (!observerPlayerId)
        return false;
    request.observerPlayerId = *observerPlayerId;
    const auto observerState = service.views().findObserver(observer);
    if (observerState == nullptr)
        return false;
    // 观察者实体必在自己的权威视野中；客户端 sink 仍会再次验证 session epoch。
    if (service.pushToClient(request) != AoiPushResult::kAccepted)
        return false;
    AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::Commit);
    return service.views().commitAccepted(observer, token, accepted);
}
}

bool ProcessAoiClientObserver(AoiService& service, EntityId observer)
{
    const auto token = service.views().prepareSend(observer);
    if (!token)
        return false;

    const auto current = CurrentView(service, observer);
    const auto* baseline=service.views().borrowCommittedBaseline(observer);
    if (baseline == nullptr)
        return false;

    if (service.views().needResync(observer))
    {
        const auto frozen = [&] {
            AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::BuildDiff);
            return ToBaseline(current);
        }();
        constexpr uint32_t kPartSize = 128;
        const uint32_t partCount = std::max<uint32_t>(1, (frozen.size() + kPartSize - 1) / kPartSize);
        const auto snapshot = service.views().beginSnapshot(observer, frozen, partCount);
        if (!snapshot)
            return false;

        std::vector<std::pair<EntityId, SentEntityState>> ordered(frozen.begin(), frozen.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (uint32_t part = 0; part < partCount; ++part)
        {
            gs::SCPushViewSnapshotPart message;
            message.set_snapshot_id(snapshot->snapshotId);
            message.set_view_seq(snapshot->viewSeq);
            message.set_part_index(part);
            message.set_part_count(partCount);
            const size_t begin = static_cast<size_t>(part) * kPartSize;
            const size_t end = std::min(ordered.size(), begin + kPartSize);
            for (size_t i = begin; i < end; ++i)
            {
                AoiEntity entity;
                entity.entityId = ordered[i].first;
                entity.kind = ordered[i].second.kind;
                entity.businessId = ordered[i].second.businessId;
                entity.entityGeneration = ordered[i].second.entityGeneration;
                entity.spatialVersion = ordered[i].second.spatialVersion;
                entity.x = static_cast<Position>(ordered[i].second.x);
                entity.y = static_cast<Position>(ordered[i].second.y);
                entity.dir = ordered[i].second.dir;
                FillProto(entity, message.add_entities());
            }

            AoiClientPushRequest request;
            request.clientViewEpoch = snapshot->clientViewEpoch;
            request.module = Module::AOI;
            request.method = Method::AOI_VIEW_SNAPSHOT;
            {
                AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::Serialize);
                request.body = message.SerializeAsString();
            }
            if (service.diagnostics()) 
                service.diagnostics()->add(AoiDiagnostics::Counter::PushEntities, end - begin);
            const auto observerPlayerId = service.BusinessIdOf(observer);
            request.observerPlayerId = observerPlayerId.value_or(0);
            if (request.observerPlayerId == 0 ||
                service.pushToClient(request) != AoiPushResult::kAccepted)
            {
                service.views().abortSnapshot(*snapshot);
                service.OnClientSendFailed(observer, snapshot->clientViewEpoch);
                return false;
            }
            service.views().markSnapshotPartAccepted(*snapshot, part);
        }
        AoiDiagnostics::Scope timing(service.diagnostics(), AoiDiagnostics::Stage::Commit);
        return service.views().commitSnapshot(*snapshot);
    }

    // SendDelta 在调用输出回调或提交之前完成所有基线读取。
    return SendDelta(service, observer, *token, current, *baseline);
}