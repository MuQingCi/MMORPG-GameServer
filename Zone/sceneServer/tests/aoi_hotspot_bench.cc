#include "service/aoi/aoiClientView.h"
#include "player/player.h"
#include "metrics/queueDiagnosticsJson.h"
#include "gameProto.pb.h"
#include "common/msg.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {
struct Sink final : AoiPushSink {
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, gs::ViewEntity>> views;
    AoiPushResult sendToClient(const AoiClientPushRequest& request) override {
        auto& view = views[request.observerPlayerId];
        if (request.method == Method::AOI_VIEW_SNAPSHOT) {
            gs::SCPushViewSnapshotPart message;
            if (!message.ParseFromString(request.body)) throw std::runtime_error("snapshot parse");
            if (message.part_index() == 0) view.clear();
            for (const auto& entity : message.entities()) view[entity.entity_id()] = entity;
        } else if (request.method == Method::AOI_VIEW_DELTA) {
            gs::SCPushViewDelta message;
            if (!message.ParseFromString(request.body)) throw std::runtime_error("delta parse");
            for (const auto& entity : message.entered()) view[entity.entity_id()] = entity;
            for (const auto& entity : message.moved()) view[entity.entity_id()] = entity;
            for (const auto& entity : message.left()) view.erase(entity.entity_id());
        } else throw std::runtime_error("unexpected AOI method");
        return AoiPushResult::kAccepted;
    }
    AoiWorkerPublishResult publishToWorkers(const AoiWorkerPublishRequest&) override { return {}; }
};
}

int main(int argc, char** argv)
{
    try {
        if (argc != 6) throw std::runtime_error("usage: entities dense|sparse moving ticks warmup");
        const int count = std::stoi(argv[1]), ticks = std::stoi(argv[4]), warmup = std::stoi(argv[5]);
        const std::string layout = argv[2];
        const bool moving = std::stoi(argv[3]) != 0;
        if (count < 1 || count > 512 || ticks < 1 || warmup < 0 ||
            (layout != "dense" && layout != "sparse")) throw std::runtime_error("invalid workload");
        Map map;
        std::string error;
        if (!Map::Load(std::string(SCENE_SOURCE_DIR) + "/config/map.yaml", 50, map, error))
            throw std::runtime_error(error);
        Sink sink;
        AoiConfig cfg;
        AoiServiceHooks hooks;
        hooks.processObserver = ProcessAoiClientObserver;
        AoiService service(0, cfg, sink, map, hooks);
        std::vector<Player> players(count);
        for (int i = 0; i < count; ++i) {
            players[i].set_id(1000+i);
            const int spacing = layout == "dense" ? 1 : 75;
            players[i].setPos(10+(i%24)*spacing, 10+(i/24)*spacing, 0);
            if (service.OnEntityEnter(players[i]) != AoiService::ApplyResult::kApplied ||
                !service.OnPlayerOnline(1000+i, 1, 1)) throw std::runtime_error("setup failed");
        }
        AoiDiagnostics metrics;
        uint64_t mismatches = 0;
        auto runTick = [&](int t, bool verify) {
            if (moving) for (auto& p : players) {
                p.setPos(p.x() + (t%2 ? 1 : -1), p.y(), t%4);
                service.OnLocalEntityMoved(p);
            }
            service.OnTick(int64_t(t)*cfg.tickMs);
            if (!verify) return;
            // 独立的几何参考实现，不调用生产代码的 ViewOf 查询。
            for (const auto& observer : players) {
                const auto& actual = sink.views[observer.id()];
                size_t expected = 0;
                for (const auto& target : players) {
                    if (observer.id() == target.id()) continue;
                    if (std::abs(observer.x()/50-target.x()/50) > 1 ||
                        std::abs(observer.y()/50-target.y()/50) > 1) continue;
                    ++expected;
                    const auto id = service.EntityOf(target.id());
                    const auto it = actual.find(id.value_or(0));
                    if (it == actual.end() || it->second.business_id() != target.id() ||
                        it->second.x() != target.x() || it->second.y() != target.y() ||
                        it->second.dir() != target.dir()) ++mismatches;
                }
                if (actual.size() != expected) ++mismatches;
            }
        };
        for (int i = 0; i < warmup; ++i) runTick(i, true);
        service.setDiagnostics(&metrics);
        const auto begin = AoiDiagnostics::nowUs();
        for (int i = warmup; i < warmup+ticks; ++i) runTick(i, false);
        const auto elapsed = AoiDiagnostics::nowUs() - begin;
        service.setDiagnostics(nullptr);
        // 验证最终状态，不增加额外状态转换或计入测量的查询。
        for (const auto& observer : players) {
            std::vector<AoiEntity> expected;
            service.ViewOf(*service.EntityOf(observer.id()), expected);
            const auto& actual = sink.views[observer.id()];
            if (expected.size() != actual.size()) ++mismatches;
            for (const auto& entity : expected) {
                const auto it = actual.find(entity.entityId);
                if (it == actual.end() || it->second.business_id() != entity.businessId ||
                    it->second.entity_generation() != entity.entityGeneration ||
                    it->second.spatial_version() != entity.spatialVersion ||
                    it->second.entity_kind() != static_cast<uint32_t>(entity.kind) ||
                    it->second.x() != static_cast<int32_t>(entity.x) ||
                    it->second.y() != static_cast<int32_t>(entity.y) || it->second.dir() != entity.dir)
                    ++mismatches;
            }
        }
        QueueDiagnostics::Snapshot envelope;
        envelope.aoi.push_back(metrics.snapshot());
        const auto serialized = serializeQueueDiagnostics(envelope);
        const auto start = serialized.find("\"aoi\":[") + 7;
        const auto finish = serialized.rfind("]}");
        std::cout << "{\"entities\":" << count << ",\"ticks\":" << ticks
                  << ",\"layout\":\"" << layout << "\",\"moving\":" << moving
                  << ",\"elapsedUs\":" << elapsed << ",\"referenceMismatches\":" << mismatches
                  << ",\"aoi\":" << serialized.substr(start, finish-start) << "}\n";
        return mismatches ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}