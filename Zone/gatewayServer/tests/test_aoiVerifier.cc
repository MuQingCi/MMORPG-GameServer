#include "test_util.h"
#include "aoiVerifier.h"

TEST(AoiVerifierRequiresCompleteSnapshotAndContiguousSequence)
{
    AoiVerifier verifier;
    CHECK(!verifier.complete());
    gs::SCPushViewSnapshotPart part;
    part.set_snapshot_id(1); part.set_view_seq(1); part.set_part_count(2);
    auto* entity = part.add_entities(); entity->set_entity_id(1); entity->set_business_id(100);
    CHECK(verifier.snapshot(part));
    CHECK(!verifier.complete());
    part.clear_entities(); part.set_part_index(1);
    CHECK(verifier.snapshot(part));
    CHECK(verifier.complete());
    gs::SCPushViewDelta delta; delta.set_view_seq(2);
    auto* moved = delta.add_moved(); moved->set_entity_id(1); moved->set_business_id(100);
    CHECK(verifier.delta(delta));
    delta.set_view_seq(4);
    CHECK(!verifier.delta(delta));
    CHECK(!verifier.complete());
}

TEST(AoiVerifierRejectsDuplicatePartsAndUnknownMove)
{
    AoiVerifier verifier;
    gs::SCPushViewSnapshotPart part;
    part.set_snapshot_id(1); part.set_view_seq(1); part.set_part_count(1);
    CHECK(verifier.snapshot(part));
    gs::SCPushViewDelta delta; delta.set_view_seq(2);
    delta.add_moved()->set_entity_id(9);
    CHECK(!verifier.delta(delta));
    CHECK(!verifier.complete());
    AoiVerifier other;
    part.set_part_count(2);
    CHECK(other.snapshot(part));
    CHECK(!other.snapshot(part));
}