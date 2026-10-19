#pragma once
#include "gameProto.pb.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

// 可选的负载客户端检查器；后续完整快照可以恢复序列缺口。
class AoiVerifier {
public:
    bool complete() const { return complete_; }
    uint64_t errors() const { return errors_; }
    uint64_t sequence() const { return sequence_; }
    const auto& entities() const { return entities_; }
    bool snapshot(const gs::SCPushViewSnapshotPart& part) {
        if (!part.snapshot_id() || !part.view_seq() || !part.part_count() ||
            part.part_count() > 4096 || part.part_index() >= part.part_count()) return fail();
        if (part.snapshot_id() != snapshotId_) {
            if (part.part_index() != 0) return fail();
            snapshotId_ = part.snapshot_id();
            snapshotSeq_ = part.view_seq();
            parts_.assign(part.part_count(), false);
            pending_.clear(); complete_ = false;
        }
        if (part.part_count() != parts_.size() || part.view_seq() != snapshotSeq_ ||
            parts_[part.part_index()]) return fail();
        for (const auto& entity : part.entities())
            if (!entity.entity_id() || !pending_.emplace(entity.entity_id(), entity).second) return fail();
        parts_[part.part_index()] = true;
        for (bool done : parts_) if (!done) return true;
        entities_.swap(pending_); pending_.clear();
        sequence_ = snapshotSeq_; complete_ = true;
        return true;
    }
    bool delta(const gs::SCPushViewDelta& delta) {
        if (!complete_ || delta.view_seq() != sequence_ + 1) return fail();
        auto next = entities_;
        for (const auto& e : delta.entered()) {
            if (!e.entity_id()) return fail();
            next[e.entity_id()] = e;
        }
        for (const auto& e : delta.moved()) {
            auto it = next.find(e.entity_id());
            if (it == next.end() || it->second.entity_generation() != e.entity_generation()) return fail();
            it->second = e;
        }
        for (const auto& e : delta.left()) {
            auto it = next.find(e.entity_id());
            if (it == next.end() || it->second.entity_generation() != e.entity_generation()) return fail();
            next.erase(it);
        }
        entities_.swap(next); sequence_ = delta.view_seq();
        return true;
    }
private:
    bool fail() { ++errors_; complete_ = false; return false; }
    bool complete_ = false;
    uint64_t errors_ = 0, sequence_ = 0, snapshotId_ = 0, snapshotSeq_ = 0;
    std::vector<bool> parts_;
    std::unordered_map<uint64_t, gs::ViewEntity> entities_, pending_;
};