#include <gtest/gtest.h>
#include <map>
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include <iterator>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/adjacency/CoveredAdjacencyCandidate.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/index/FixedBTreeMapMutation.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
namespace {
constexpr uint64_t CANDIDATE_INDEX = 0x434F564552ULL; // laboratory-owned, not reserved/adopted
using Candidate = CoveredAdjacencyCandidate;
class CoveredAdjacencyCandidateTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    uint64_t source = 0, target = 0;
    std::vector<EntityRef> edges;
    NativeGraphWritePlannerOptions options() const {
        NativeGraphWritePlannerOptions o; o.first_slot = PhysicalSlotId(40); o.last_slot = PhysicalSlotId(100);
        o.entity_pool_owner_id = 901; o.property_roots = {o.first_slot, o.last_slot, 902}; return o;
    }
    NativeGraphMutationExecutorOptions logicalLimits() const {
        NativeGraphMutationExecutorOptions limits;
    limits.maximum_index_definitions = limits.maximum_constraint_definitions = 8;
    limits.maximum_constraint_entity_scan = 128;
    limits.maximum_constraint_support_collisions = 8;
    limits.maximum_vector_records = 64; limits.maximum_vector_pages = 8;
    limits.vector_delta_rebuild_threshold = 8; limits.maximum_vector_delta_overlay_entries = 16;
    limits.maximum_vector_delta_pages = 4;
    limits.covered_adjacency_candidate_index = CANDIDATE_INDEX;
    limits.maximum_covered_adjacency_changes = 2;
    limits.covered_adjacency_workspace_bytes = 1024 * 1024;
        return limits;
    }
    NativeGraphMutationExecutor logicalExecutor(NativeGraphWritePlanner &planner, PageBackedGraphReadAdapter &graph,
        NativeGraphMutationExecutorOptions limits) {
    NativeGraphMutationTokenResolvers tokens;
    tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind, std::string_view) -> std::optional<uint64_t> { return 1; };
    tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return 1; };
    tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t) -> std::optional<std::string> { return "value"; };
    return NativeGraphMutationExecutor(planner, graph, *btree_pages_, *vector_pages_, *vector_manifests_, *metadata_,
        tokens, [](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
        [](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, limits);
    }
    void verifyDerivedPublication(BootstrapPublishFailpoint failpoint);
    void seed() {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); source = tx.createNode({}); target = tx.createNode({});
        std::vector<BTreePageEntry> records;
        for (unsigned i = 0; i < 90; ++i) {
            const auto neighbor = i == 89 ? source : target;
            const auto id = tx.createEdge(source, neighbor, i % 3);
            edges.push_back({id, true, source, neighbor, i % 3});
            for (auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                const auto e = Candidate::entry(direction, id, source, neighbor, i % 3);
                const auto record = Candidate::Map::encode(e.key, e.value);
                records.push_back({std::vector<std::byte>(record.begin(), record.end()), 0});
            }
        }
        std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        const OrderedBTreeEntrySource input = [&records]() -> OrderedBTreeEntryCursor {
            return [&records, i = size_t{0}]() mutable -> std::optional<BTreePageEntry> {
                if (i == records.size()) return {}; return records[i++];
            };
        };
        const BulkBTreeBuildOptions build{PhysicalSlotId(40), PhysicalSlotId(100), CANDIDATE_INDEX,
            records.size(), 32, BTreeLeafValueLayout::BLVL_EMPTY};
        tx.stageScalarIndexCreate(*btree_pages_, BulkBTreeBuilder::planNativeTree(*btree_pages_, *metadata_,
            tx.baseSnapshot(), CANDIDATE_INDEX, input, build));
        ASSERT_TRUE(tx.commit().committed());
    }
    std::vector<uint64_t> scanAll(const MetadataSnapshotPin &pin, GlobalAdjacencyDirection direction, uint64_t node) {
        std::vector<uint64_t> ids; std::optional<AdjacencyTopologyIdentity> continuation;
        do {
            const auto page = Candidate::scan(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX,
                direction, node, 32, continuation);
            EXPECT_LE(page.candidate_records, 33U);
            for (const auto &edge : page.topology.entries()) ids.push_back(edge.entity_id);
            continuation = page.continuation;
        } while (continuation);
        return ids;
    }
    void eraseBoth(NativeGraphWriteTransaction &tx, std::span<const EntityRef> removed) {
        for (const auto &edge : removed) tx.eraseEdge(edge.entity_id);
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, removed.size(), memory);
    }
};
TEST_F(CoveredAdjacencyCandidateTest, NativeDoubleDirectionCreateDeleteRetainsOldPinsAndReopens) {
    seed(); auto old = metadata_->openSnapshot();
    auto ids = scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source); ASSERT_EQ(ids.size(), 90U);
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_IN, target).size(), 89U);
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_IN, source), (std::vector<uint64_t>{edges.back().entity_id}));
    const std::array removed{edges.front(), edges[2], edges.back()};
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); eraseBoth(tx, removed); ASSERT_TRUE(tx.commit().committed());
    }
    auto fresh = metadata_->openSnapshot(); auto expected = ids;
    for (const auto &edge : removed) std::erase(expected, edge.entity_id);
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source), ids);
    EXPECT_EQ(scanAll(fresh, GlobalAdjacencyDirection::GAD_OUT, source), expected);
    EXPECT_EQ(scanAll(fresh, GlobalAdjacencyDirection::GAD_IN, target).size(), 87U);
    EXPECT_TRUE(scanAll(fresh, GlobalAdjacencyDirection::GAD_IN, source).empty());
    old.release(); fresh.release(); reopenNativeStores();
    auto reopened = metadata_->openSnapshot(); EXPECT_EQ(scanAll(reopened, GlobalAdjacencyDirection::GAD_OUT, source), expected);
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); tx.stageScalarIndexDrop(*btree_pages_, CANDIDATE_INDEX, 32);
        ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_EQ(scanAll(reopened, GlobalAdjacencyDirection::GAD_OUT, source), expected);
    const auto dropped = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->findIndexRootOptional(dropped, CANDIDATE_INDEX));
    EXPECT_THROW(Candidate::scan(*btree_pages_, *entities_, *metadata_, dropped, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 32), FormatException);
}
TEST_F(CoveredAdjacencyCandidateTest, ContinuationsBindTheNativePinAndSeekHubRangesWithoutPrefixRescans) {
    seed(); const auto pin = metadata_->openSnapshot();
    const auto first = Candidate::scan(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 64);
    ASSERT_TRUE(first.continuation); EXPECT_EQ(first.topology.entries().size(), 64U); EXPECT_EQ(first.candidate_records, 65U);
    for (unsigned field = 0; field < 6; ++field) {
        auto wrong = *first.continuation;
        if (field == 0) ++wrong.database_id;
        if (field == 1) ++wrong.snapshot_generation;
        if (field == 2) ++wrong.node_id;
        if (field == 3) wrong.direction = LogicalGraphReadDirection::LGRD_INCOMING;
        if (field == 4) wrong.expected_first_edge_id = wrong.after_edge_id;
        if (field == 5) ++wrong.expected_first_edge_id;
        EXPECT_THROW(Candidate::scan(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX,
            GlobalAdjacencyDirection::GAD_OUT, source, 64, wrong), std::exception);
    }
    const auto last = Candidate::scan(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 64, first.continuation);
    EXPECT_EQ(last.topology.entries().size(), 26U); EXPECT_EQ(last.candidate_records, 26U); EXPECT_FALSE(last.continuation);
    EXPECT_EQ(last.topology.entries().front().entity_id, edges[64].entity_id);
}
TEST_F(CoveredAdjacencyCandidateTest, ValidChecksumsCannotHideAWrongLookaheadFromMainEdgeVerification) {
    seed(); const auto old = metadata_->openSnapshot();
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); graph::query::execution::MaterializedByteBudget memory(65536);
        FixedBTreeMapMutation<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 16> changes(tx, *btree_pages_, *metadata_, CANDIDATE_INDEX, 1, memory);
        const auto &edge = edges[1]; const auto corrupt = Candidate::entry(GlobalAdjacencyDirection::GAD_OUT,
            edge.entity_id, edge.source_node_id, edge.target_node_id, edge.type_token + 1U);
        changes.set(corrupt.key, corrupt.value); changes.stage(); ASSERT_TRUE(tx.commit().committed());
    }
    const auto fresh = metadata_->openSnapshot();
    EXPECT_THROW(Candidate::scan(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 1), std::runtime_error);
    const auto unaffected = Candidate::scan(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 1);
    ASSERT_EQ(unaffected.topology.entries().size(), 1U); EXPECT_TRUE(unaffected.continuation);
}
TEST_F(CoveredAdjacencyCandidateTest, InterruptedNativePublicationRecoversMatchingMainAndBothCandidateDirections) {
    seed();
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
                                BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        {
            NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
            auto tx = planner.beginTransaction(); eraseBoth(tx, std::span(edges).first(1));
            MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
            EXPECT_THROW(tx.commit(publication), BootstrapPublishInterrupted);
        }
        reopenNativeStores(); const auto pin = metadata_->openSnapshot();
        const auto expected = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE ? 89U : 90U;
        EXPECT_EQ(scanAll(pin, GlobalAdjacencyDirection::GAD_OUT, source).size(), expected);
        EXPECT_EQ(scanAll(pin, GlobalAdjacencyDirection::GAD_IN, target).size(), expected - 1U);
        if (expected == 90) EXPECT_NO_THROW(entities_->readEdge(pin, edges[0].entity_id));
        else EXPECT_THROW(entities_->readEdge(pin, edges[0].entity_id), FormatException);
    }
}
TEST_F(CoveredAdjacencyCandidateTest, GlobalReconciliationRequiresCompleteBoundedScansAndPublishesNoPartialReport) {
    seed(); const auto pin = metadata_->openSnapshot(); const auto bytes = io_->fileSize(), epoch = io_->dataMutationEpoch();
    const auto report = Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 90, 64);
    EXPECT_EQ(report.database_id, pin.superblock().database_id); EXPECT_EQ(report.generation, pin.superblock().generation);
    EXPECT_EQ(report.edges, 90U); EXPECT_EQ(report.candidate_entries, 180U); EXPECT_GT(report.edge_metadata_pages, 0U);
    EXPECT_THROW(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 89, 64), std::length_error);
    EXPECT_THROW(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 90, 1), FormatException);
    struct Cancel final : CancellationCheckpoint {
        mutable unsigned calls = 0;
        void check() const override { if (++calls == 3) throw std::runtime_error("cancel global candidate reconciliation"); }
    } cancellation;
    std::optional<Candidate::IntegrityReport> partial;
    EXPECT_THROW(partial.emplace(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin,
        CANDIDATE_INDEX, 90, 64, {}, &cancellation)), std::runtime_error);
    EXPECT_FALSE(partial);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 90, 64).edges, 90U);
    EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(CoveredAdjacencyCandidateTest, AuthoritativeEdgeCursorResumesAfterLogicalIdsUnderAnIndependentMetadataBound) {
    seed(); auto pin = metadata_->openSnapshot();
    std::vector<uint64_t> ids; size_t metadataPages = 0;
    EXPECT_FALSE(metadata_->visitEdgePageLocationsAfter(pin, 0, 64,
        [&](const EntityPageLocation &location) { ids.push_back(location.entity_id); return ids.size() < 32; }, &metadataPages));
    ASSERT_EQ(ids.size(), 32U); EXPECT_GT(metadataPages, 0U);
    const auto after = ids.back();
    EXPECT_TRUE(metadata_->visitEdgePageLocationsAfter(pin, after, 64,
        [&](const EntityPageLocation &location) { ids.push_back(location.entity_id); return true; }, &metadataPages));
    ASSERT_EQ(ids.size(), edges.size());
    for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], edges[i].entity_id);
    EXPECT_THROW(metadata_->visitEdgePageLocationsAfter(pin, 0, 0, [](const auto &) { return true; }), FormatException);
    EXPECT_THROW(metadata_->visitEdgePageLocationsAfter(pin, 0, 64, {}), FormatException);
    size_t callbacks = 0;
    EXPECT_TRUE(metadata_->visitEdgePageLocationsAfter(pin, UINT64_MAX, 64,
        [&](const auto &) { ++callbacks; return true; }, &metadataPages));
    EXPECT_EQ(callbacks, 0U); EXPECT_EQ(metadataPages, 0U);
    pin.release();
    EXPECT_THROW(metadata_->visitEdgePageLocationsAfter(pin, 0, 64, [](const auto &) { return true; }), FormatException);
}
TEST_F(CoveredAdjacencyCandidateTest, GlobalReconciliationFindsAMissingReverseEntryBeyondThePointReadContract) {
    seed(); const auto old = metadata_->openSnapshot();
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); graph::query::execution::MaterializedByteBudget memory(65536);
        FixedBTreeMapMutation<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 16> changes(tx, *btree_pages_, *metadata_, CANDIDATE_INDEX, 1, memory);
        changes.erase(Candidate::key(GlobalAdjacencyDirection::GAD_IN, edges[0].target_node_id, edges[0].entity_id));
        changes.stage(); ASSERT_TRUE(tx.commit().committed());
    }
    const auto fresh = metadata_->openSnapshot();
    const auto outgoing = Candidate::scan(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 32);
    EXPECT_EQ(outgoing.topology.entries().size(), 32U);
    EXPECT_THROW(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX, 90, 64), FormatException);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX, 90, 64).candidate_entries, 180U);
}
TEST_F(CoveredAdjacencyCandidateTest, GlobalReconciliationRejectsAnOrphanWithValidNativeTreeChecksums) {
    seed();
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction();
        const auto orphan = Candidate::entry(GlobalAdjacencyDirection::GAD_OUT, edges.back().entity_id + 10, source, target, 0);
        const auto record = Candidate::Map::encode(orphan.key, orphan.value);
        tx.stageScalarIndexUpsert(*btree_pages_, CANDIDATE_INDEX, record, 0, BTreeLeafValueLayout::BLVL_EMPTY);
        ASSERT_TRUE(tx.commit().committed());
    }
    const auto pin = metadata_->openSnapshot();
    EXPECT_THROW(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 100, 64), FormatException);
}
TEST_F(CoveredAdjacencyCandidateTest, GlobalReconciliationRejectsDuplicateLogicalKeysWithDifferentSuffixes) {
    seed();
    {
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
        auto tx = planner.beginTransaction(); const auto &edge = edges[0];
        const auto duplicate = Candidate::entry(GlobalAdjacencyDirection::GAD_OUT, edge.entity_id, source, target, edge.type_token + 1);
        const auto record = Candidate::Map::encode(duplicate.key, duplicate.value);
        tx.stageScalarIndexUpsert(*btree_pages_, CANDIDATE_INDEX, record, 0, BTreeLeafValueLayout::BLVL_EMPTY);
        ASSERT_TRUE(tx.commit().committed());
    }
    const auto pin = metadata_->openSnapshot();
    EXPECT_THROW(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 100, 64), FormatException);
}
TEST_F(CoveredAdjacencyCandidateTest, TypedRelocationRetainsOldPinsAndSafelyReusesTheReleasedSourceSlot) {
    seed();
    IndexRootLocation sourceRoot; CatalogEntry sourcePool;
    {
        const auto pin = metadata_->openSnapshot(); sourceRoot = metadata_->findIndexRoot(pin, CANDIDATE_INDEX);
        sourcePool = metadata_->findCatalog(pin, metadata_->findPageLocation(pin, sourceRoot.root_page_id).segment_id);
    }
    const auto allocateSupport = [&](uint64_t index, uint64_t logicalId, Generation generation, PhysicalSlotId slot) {
        const auto pin = metadata_->openSnapshot();
        const LogSequenceNumber lsn(pin.superblock().checkpoint_lsn.value() + 1);
        SlottedPagePoolAllocationRequest request;
        request.segment_id = SegmentId(logicalId); request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
        request.pool_owner_id = index; request.generation = generation; request.page_id = PageId(logicalId);
        request.page_owner_id = index; request.page_owner_kind = PageOwnerKind::POK_INDEX;
        PageOwnerHeader owner{request.page_id, index, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, generation, lsn};
        BTreePageHeader header; header.index_id = index; header.page_id = request.page_id;
        header.generation = generation; header.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
        auto allocation = btree_pages_->allocate(request, BTreePage::create(owner, header), slot, slot, lsn);
        if (!allocation) throw std::runtime_error("support pool has no destination slot");
        const IndexRootLocation root{index, request.page_id, generation};
        const auto next = metadata_->openSnapshot().superblock();
        static_cast<void>(btree_pages_->publishRoots(std::span(&root, 1), {}, LogSequenceNumber(next.checkpoint_lsn.value() + 1)));
    };
    allocateSupport(17, 900, Generation(2), PhysicalSlotId(110));
    auto old = metadata_->openSnapshot();
    const auto first = Candidate::scan(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 32);
    ASSERT_TRUE(first.continuation);
    RelocatableCompactionCoordinator coordinator(*slotted_pages_, *btree_pages_, *metadata_);
    const auto compacted = coordinator.compactBTreePool({sourcePool.segment_id, SegmentId(900),
        RSS_PAGE_POOL_DATA_FRAME_COUNT, LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1)});
    EXPECT_EQ(compacted.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
    EXPECT_TRUE(compacted.source_empty); EXPECT_TRUE(compacted.source_retired); EXPECT_FALSE(compacted.source_released);
    EXPECT_GT(compacted.moved_pages, 0U);
    {
        const auto fresh = metadata_->openSnapshot(); const auto root = metadata_->findIndexRoot(fresh, CANDIDATE_INDEX);
        EXPECT_EQ(root.root_page_id, sourceRoot.root_page_id); EXPECT_EQ(root.root_generation, Generation(2));
        EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX, 90, 64).edges, 90U);
        EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source).size(), 90U);
        EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX, 90, 64).edges, 90U);
        EXPECT_THROW(Candidate::scan(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX,
            GlobalAdjacencyDirection::GAD_OUT, source, 32, first.continuation), FormatException);
    }
    EXPECT_FALSE(coordinator.releaseRetiredSlottedPool(sourcePool.segment_id, sourcePool.generation, compacted.next_lsn));
    old.release();
    EXPECT_TRUE(coordinator.releaseRetiredSlottedPool(sourcePool.segment_id, sourcePool.generation, compacted.next_lsn));
    {
        const auto pin = metadata_->openSnapshot(); EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(pin, sourcePool.physical_slot));
    }
    allocateSupport(18, 901, Generation(sourcePool.generation.value() + 1), sourcePool.physical_slot);
    {
        const auto pin = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(pin, sourcePool.physical_slot));
        EXPECT_EQ(metadata_->findCatalog(pin, SegmentId(901)).physical_slot, sourcePool.physical_slot);
        EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 90, 64).candidate_entries, 180U);
    }
    reopenNativeStores(); const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(scanAll(pin, GlobalAdjacencyDirection::GAD_OUT, source).size(), 90U);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 90, 64).edges, 90U);
}
} // namespace

TEST_F(CoveredAdjacencyCandidateTest, FinalStateCoordinatorMaintainsBothDirectionsAndSealsFurtherStagingAcrossMove) {
    seed(); auto old = metadata_->openSnapshot(); const auto oldIds = scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto tx = planner.beginTransaction(); tx.eraseEdge(edges[0].entity_id); tx.eraseEdge(edges.back().entity_id);
    const auto added = tx.createEdge(source, source, 0);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 3, memory);
    EXPECT_THROW((void)tx.createEdge(source, target, 1), FormatException);
    EXPECT_THROW(tx.eraseEdge(edges[2].entity_id), FormatException);
    EXPECT_THROW(tx.stageScalarIndexErase(*btree_pages_, CANDIDATE_INDEX, {}), FormatException);
    auto moved = std::move(tx); EXPECT_FALSE(tx.isActive());
    EXPECT_THROW((void)moved.createEdge(source, target, 1), FormatException);
    ASSERT_TRUE(moved.commit().committed());
    auto current = metadata_->openSnapshot(); auto expected = oldIds;
    std::erase(expected, edges[0].entity_id); std::erase(expected, edges.back().entity_id); expected.push_back(added);
    EXPECT_EQ(scanAll(current, GlobalAdjacencyDirection::GAD_OUT, source), expected);
    EXPECT_EQ(scanAll(current, GlobalAdjacencyDirection::GAD_IN, source), (std::vector<uint64_t>{added}));
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source), oldIds);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, current, CANDIDATE_INDEX, 128, 8192).edges, 89U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(CoveredAdjacencyCandidateTest, CoordinatorBoundQuotaAndCancellationRefuseBeforeStagingAndCanRetry) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto tx = planner.beginTransaction(); tx.eraseEdge(edges[0].entity_id); tx.eraseEdge(edges[1].entity_id);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException);
    for (const auto reserved : {uint64_t{0}, uint64_t{1} << 62U, uint64_t{1} << 63U})
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(reserved, 2, memory), FormatException);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 2, tiny), graph::query::execution::MaterializedByteLimitExceeded);
    struct Stop final : CancellationCheckpoint { void check() const override { throw std::runtime_error("stop"); } } stop;
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 2, memory, &stop), std::runtime_error);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW((void)tx.commit(), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 2, memory); ASSERT_TRUE(tx.commit().committed());
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192).edges, 88U);
}
TEST_F(CoveredAdjacencyCandidateTest, CoordinatorRefusesAValidChecksumWrongOldDirectionWithoutMaskingCorruption) {
    seed(); const auto edge = edges[1];
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    {
        auto damage = planner.beginTransaction(); graph::query::execution::MaterializedByteBudget scratch(65536);
        FixedBTreeMapMutation<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 16> changes(damage, *btree_pages_, *metadata_, CANDIDATE_INDEX, 1, scratch);
        const auto wrong = Candidate::entry(GlobalAdjacencyDirection::GAD_IN, edge.entity_id, source, target, edge.type_token + 1U);
        changes.set(wrong.key, wrong.value); changes.stage(); ASSERT_TRUE(damage.commit().committed());
    }
    auto tx = planner.beginTransaction(); tx.eraseEdge(edge.entity_id);
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException);
    EXPECT_TRUE(tx.isActive()); EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(entities_->readEdge(metadata_->openSnapshot(), edge.entity_id).type_id, edge.type_token);
    EXPECT_THROW((void)Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192), FormatException);
}
TEST_F(CoveredAdjacencyCandidateTest, CoordinatorUsesFinalEdgeStateAndRefusesManualCandidateDeltaMixing) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(edges[0].entity_id);
        const auto record = Candidate::Map::encode(Candidate::key(GlobalAdjacencyDirection::GAD_OUT, source, edges[0].entity_id), {});
        tx.stageScalarIndexErase(*btree_pages_, CANDIDATE_INDEX, record, BTreeLeafValueLayout::BLVL_EMPTY);
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException); tx.rollback();
    }
    auto tx = planner.beginTransaction(); const auto discarded = tx.createEdge(source, target, 7); tx.eraseEdge(discarded);
    tx.eraseEdge(edges[0].entity_id); tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory);
    ASSERT_TRUE(tx.commit().committed());
    const auto report = Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192);
    EXPECT_EQ(report.edges, 89U); EXPECT_EQ(report.candidate_entries, 178U); EXPECT_EQ(memory->used(), 0U);
}

void CoveredAdjacencyCandidateTest::verifyDerivedPublication(BootstrapPublishFailpoint failpoint) {
    seed();
    constexpr uint64_t typeIndexId = CANDIDATE_INDEX + 1;
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_);
    std::vector<BTreePageEntry> records;
    for (const auto &edge : edges) {
        if (!edge.type_token) continue; // Untyped edges have no type-index key.
        records.push_back({IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t(edge.type_token)), edge.entity_id), edge.entity_id});
    }
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
    const OrderedBTreeEntrySource input = [&records]() -> OrderedBTreeEntryCursor {
        return [&records, i = size_t{0}]() mutable -> std::optional<BTreePageEntry> {
            if (i == records.size()) return {}; return records[i++];
        };
    };
    {
        auto tx = planner.beginTransaction();
        const BulkBTreeBuildOptions build{PhysicalSlotId(40), PhysicalSlotId(100), typeIndexId,
            records.size(), 32, BTreeLeafValueLayout::BLVL_U64};
        tx.stageScalarIndexCreate(*btree_pages_, BulkBTreeBuilder::planNativeTree(*btree_pages_, *metadata_,
            tx.baseSnapshot(), typeIndexId, input, build));
        auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90});
        session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 0, 30);
        session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 1, 30);
        session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 2, 30);
        session.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE);
        session.finish();
        ASSERT_TRUE(tx.commit().committed());
    }
    IndexDefinition definition; definition.index_id = typeIndexId; definition.name = "covered-lab-types";
    definition.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE; definition.kind = IndexDefinitionKind::IDK_LABEL;
    NativeScalarIndexMutationPlanner scalar(*metadata_, *btree_pages_, {definition},
        [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return {}; });
    auto old = metadata_->openSnapshot();
    const auto removed = edges[1];
    const auto removedKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t(removed.type_token)), removed.entity_id);
    uint64_t created = 0;
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(removed.entity_id);
        created = tx.createEdge(source, source, 7);
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 2, memory, nullptr, &scalar, &catalog);
        EXPECT_TRUE(BTreeIndex(typeIndexId, *btree_pages_, *metadata_).find(old, removedKey));
        if (failpoint != BootstrapPublishFailpoint::BPF_NONE) {
            MetadataPublishOptions publication; publication.bootstrap_failpoint = failpoint;
            EXPECT_THROW((void)tx.commit(publication), BootstrapPublishInterrupted);
            old.release(); reopenNativeStores();
            const bool published = failpoint == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
            const auto recovered = metadata_->openSnapshot();
            SystemCatalog recoveredCatalog(*btree_pages_, *metadata_);
            BTreeIndex recoveredTypes(typeIndexId, *btree_pages_, *metadata_);
            const auto newKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t(7)), created);
            EXPECT_EQ(recoveredTypes.find(recovered, removedKey).has_value(), !published);
            EXPECT_EQ(recoveredTypes.find(recovered, newKey).has_value(), published);
            EXPECT_EQ(recoveredCatalog.readTokenCardinality(recovered, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 1), std::optional<uint64_t>(published ? 29 : 30));
            EXPECT_EQ(recoveredCatalog.readTokenCardinality(recovered, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7), std::optional<uint64_t>(published ? 1 : 0));
            EXPECT_EQ(recoveredCatalog.readGraphCardinalityStatistics(recovered), (std::optional<NativeGraphCardinalityStatistics>{{2, 90}}));
            EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, recovered, CANDIDATE_INDEX, 128, 8192).edges, 90U);
            return;
        }
        ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    }
    const auto createdKey = IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t(7)), created);
    const auto verify = [&](const MetadataSnapshotPin &pin) {
        BTreeIndex types(typeIndexId, *btree_pages_, *metadata_);
        EXPECT_FALSE(types.find(pin, removedKey)); EXPECT_EQ(types.find(pin, createdKey), std::optional(created));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(pin), (std::optional<NativeGraphCardinalityStatistics>{{2, 90}}));
        EXPECT_EQ(catalog.readTokenCardinality(pin, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 1), std::optional<uint64_t>(29));
        EXPECT_EQ(catalog.readTokenCardinality(pin, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7), std::optional<uint64_t>(1));
        EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, pin, CANDIDATE_INDEX, 128, 8192).edges, 90U);
        const auto incoming = scanAll(pin, GlobalAdjacencyDirection::GAD_IN, source);
        EXPECT_NE(std::find(incoming.begin(), incoming.end(), created), incoming.end());
    };
    verify(metadata_->openSnapshot());
    EXPECT_TRUE(BTreeIndex(typeIndexId, *btree_pages_, *metadata_).find(old, removedKey));
    EXPECT_FALSE(BTreeIndex(typeIndexId, *btree_pages_, *metadata_).find(old, createdKey));
    EXPECT_EQ(catalog.readTokenCardinality(old, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7), std::optional<uint64_t>(0));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX, 128, 8192).edges, 90U);
    old.release(); reopenNativeStores();
    SystemCatalog reopened(*btree_pages_, *metadata_);
    EXPECT_EQ(reopened.readTokenCardinality(metadata_->openSnapshot(), SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 7), std::optional<uint64_t>(1));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192).edges, 90U);
}
TEST_F(CoveredAdjacencyCandidateTest, TypeIndexAndCatalogStatisticsShareCoveredTopologyPublication) {
    verifyDerivedPublication(BootstrapPublishFailpoint::BPF_NONE);
}
TEST_F(CoveredAdjacencyCandidateTest, PartialBootstrapRetainsOldCoveredTypeIndexAndStatistics) {
    verifyDerivedPublication(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE);
}
TEST_F(CoveredAdjacencyCandidateTest, CompleteBootstrapRecoversCoveredTypeIndexAndStatisticsTogether) {
    verifyDerivedPublication(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE);
}
TEST_F(CoveredAdjacencyCandidateTest, CoordinatorPreservesUnknownTypeStatisticsAndUpdatesKnownGraphCount) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_);
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot();
    auto tx = planner.beginTransaction(); tx.eraseEdge(edges[1].entity_id);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog);
    ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    auto fresh = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(fresh), (std::optional<NativeGraphCardinalityStatistics>{{2, 89}}));
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(old), (std::optional<NativeGraphCardinalityStatistics>{{2, 90}}));
    EXPECT_FALSE(catalog.hasCompleteTokenCardinality(fresh, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE));
    EXPECT_FALSE(catalog.readTokenCardinality(fresh, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 1));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX, 128, 8192).edges, 89U);
}
TEST_F(CoveredAdjacencyCandidateTest, CoordinatorRefusesCombinedEntityBoundAndInvalidCompleteCounters) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90});
        session.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE);
        session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    const auto before = metadata_->openSnapshot().superblock();
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(edges[1].entity_id);
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog), FormatException);
        EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
        EXPECT_EQ(metadata_->openSnapshot().superblock().generation, before.generation);
    }
    {
        auto tx = planner.beginTransaction(); tx.createNode({}); tx.eraseEdge(edges[1].entity_id);
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog), FormatException);
        EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
    }
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192).edges, 90U);
}

TEST_F(CoveredAdjacencyCandidateTest, CoordinatorMaintainsNodeAndLabelCountsAcrossMixedTopologyLifecycle) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_);
    const auto labelKind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.markTokenCardinalityComplete(labelKind);
        session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto initial = metadata_->openSnapshot(); uint64_t node = 0, edge = 0;
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); node = tx.createNode(std::array<uint64_t, 3>{1, 2, 3});
        tx.replaceNodeLabels(source, std::array<uint64_t, 2>{2, 3});
        edge = tx.createEdge(node, source, 7); tx.eraseEdge(edges[1].entity_id);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 4, memory, nullptr, nullptr, &catalog);
        ASSERT_TRUE(tx.commit().committed());
    }
    auto middle = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(middle), (std::optional<NativeGraphCardinalityStatistics>{{3, 90}}));
    EXPECT_EQ(catalog.readTokenCardinality(middle, labelKind, 1), std::optional<uint64_t>(1));
    EXPECT_EQ(catalog.readTokenCardinality(middle, labelKind, 2), std::optional<uint64_t>(2));
    EXPECT_EQ(catalog.readTokenCardinality(middle, labelKind, 3), std::optional<uint64_t>(2));
    EXPECT_EQ(catalog.readTokenCardinality(initial, labelKind, 2), std::optional<uint64_t>(0));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, middle, CANDIDATE_INDEX, 128, 8192).edges, 90U);
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(edge); tx.eraseNode(node); tx.replaceNodeLabels(source, {});
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 3, memory, nullptr, nullptr, &catalog);
        ASSERT_TRUE(tx.commit().committed());
    }
    auto final = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(final), (std::optional<NativeGraphCardinalityStatistics>{{2, 89}}));
    for (uint64_t token : {1, 2, 3}) EXPECT_EQ(catalog.readTokenCardinality(final, labelKind, token), std::optional<uint64_t>(0));
    EXPECT_EQ(catalog.readTokenCardinality(middle, labelKind, 2), std::optional<uint64_t>(2));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, final, CANDIDATE_INDEX, 128, 8192).edges, 89U);
    EXPECT_EQ(memory->used(), 0U);
    initial.release(); middle.release(); final.release(); reopenNativeStores();
    SystemCatalog reopened(*btree_pages_, *metadata_); auto pin = metadata_->openSnapshot();
    EXPECT_EQ(reopened.readGraphCardinalityStatistics(pin), (std::optional<NativeGraphCardinalityStatistics>{{2, 89}}));
    EXPECT_EQ(reopened.readTokenCardinality(pin, labelKind, 2), std::optional<uint64_t>(0));
}
TEST_F(CoveredAdjacencyCandidateTest, BalancedTypeChangesPreserveTheUnchangedCatalogRoot) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.setTokenCardinality(kind, 0, 30); session.setTokenCardinality(kind, 1, 30);
        session.setTokenCardinality(kind, 2, 30); session.markTokenCardinalityComplete(kind);
        session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot(); const auto root = metadata_->findIndexRoot(old, RSS_SYSTEM_CATALOG_INDEX_ID);
    auto tx = planner.beginTransaction(); tx.eraseEdge(edges[1].entity_id); tx.createEdge(source, target, 1);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 2, memory, nullptr, nullptr, &catalog);
    ASSERT_TRUE(tx.commit().committed()); auto fresh = metadata_->openSnapshot();
    EXPECT_EQ(metadata_->findIndexRoot(fresh, RSS_SYSTEM_CATALOG_INDEX_ID), root);
    EXPECT_EQ(catalog.readTokenCardinality(fresh, kind, 1), std::optional<uint64_t>(30));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, fresh, CANDIDATE_INDEX, 128, 8192).edges, 90U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(CoveredAdjacencyCandidateTest, CompleteTypeCounterOverflowRefusesMainOnlyPublication) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.setTokenCardinality(kind, 7, UINT64_MAX - 1U);
        session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto tx = planner.beginTransaction(); const auto created = tx.createEdge(source, target, 7);
    const auto before = metadata_->openSnapshot().superblock();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog), FormatException);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX + 1, 1, memory, nullptr, nullptr, &catalog), FormatException);
    auto moved = std::move(tx);
    EXPECT_THROW(moved.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException);
    EXPECT_THROW((void)moved.commit(), FormatException); moved.rollback();
    auto current = metadata_->openSnapshot(); EXPECT_EQ(current.superblock().generation, before.generation);
    EXPECT_THROW((void)entities_->readEdge(current, created), FormatException);
    EXPECT_EQ(catalog.readTokenCardinality(current, kind, 7), std::optional<uint64_t>(UINT64_MAX - 1U));
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, current, CANDIDATE_INDEX, 128, 8192).edges, 90U);
}

TEST_F(CoveredAdjacencyCandidateTest, CoordinatorMaintainsCompleteUntypedCountsAcrossDeleteCreateAndReopen) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_);
    const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90});
        for (uint64_t type : {0U, 1U, 2U}) session.setTokenCardinality(kind, type, 30);
        session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot(); ASSERT_EQ(edges.front().type_token, 0U);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(edges.front().entity_id);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog);
        ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    }
    auto erased = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(erased), (std::optional<NativeGraphCardinalityStatistics>{{2, 89}}));
    EXPECT_EQ(catalog.readTokenCardinality(erased, kind, 0), std::optional<uint64_t>(29));
    EXPECT_EQ(catalog.readTokenCardinality(old, kind, 0), std::optional<uint64_t>(30));
    {
        auto tx = planner.beginTransaction(); (void)tx.createEdge(source, source, 0);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog);
        ASSERT_TRUE(tx.commit().committed()); EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_EQ(catalog.readTokenCardinality(metadata_->openSnapshot(), kind, 0), std::optional<uint64_t>(30));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192).edges, 90U);
    old.release(); erased.release(); reopenNativeStores();
    SystemCatalog reopened(*btree_pages_, *metadata_); const auto pin = metadata_->openSnapshot();
    EXPECT_EQ(reopened.readTokenCardinality(pin, kind, 0), std::optional<uint64_t>(30));
    EXPECT_EQ(reopened.readGraphCardinalityStatistics(pin), (std::optional<NativeGraphCardinalityStatistics>{{2, 90}}));
}
TEST_F(CoveredAdjacencyCandidateTest, CompleteUntypedCountUnderflowRefusesMainOnlyPublication) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.markTokenCardinalityComplete(kind);
        session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
    auto tx = planner.beginTransaction(); tx.eraseEdge(edges.front().entity_id);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog), FormatException);
    EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(catalog.readTokenCardinality(metadata_->openSnapshot(), kind, 0), std::optional<uint64_t>(0));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, metadata_->openSnapshot(), CANDIDATE_INDEX, 128, 8192).edges, 90U);
}
TEST_F(CoveredAdjacencyCandidateTest, NodeOnlyLifecycleUpdatesCountsWithoutRewritingTheCoveredTree) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90}); session.markTokenCardinalityComplete(kind);
        session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot(); const auto root = metadata_->findIndexRoot(old, CANDIDATE_INDEX);
    const auto location = metadata_->findPageLocation(old, root.root_page_id);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024); uint64_t created = 0;
    {
        auto tx = planner.beginTransaction(); created = tx.createNode(std::array<uint64_t, 1>{1});
        ASSERT_NO_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog));
        EXPECT_THROW((void)tx.createNode({}), FormatException); ASSERT_TRUE(tx.commit().committed());
    }
    auto middle = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(middle), (std::optional<NativeGraphCardinalityStatistics>{{3, 90}}));
    EXPECT_EQ(catalog.readTokenCardinality(middle, kind, 1), std::optional<uint64_t>(1));
    {
        auto tx = planner.beginTransaction(); tx.replaceNodeLabels(created, std::array<uint64_t, 1>{2});
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_EQ(catalog.readTokenCardinality(metadata_->openSnapshot(), kind, 1), std::optional<uint64_t>(0));
    EXPECT_EQ(catalog.readTokenCardinality(metadata_->openSnapshot(), kind, 2), std::optional<uint64_t>(1));
    {
        auto tx = planner.beginTransaction(); tx.eraseNode(created);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
    }
    const auto current = metadata_->openSnapshot();
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(current), (std::optional<NativeGraphCardinalityStatistics>{{2, 90}}));
    EXPECT_EQ(catalog.readTokenCardinality(current, kind, 2), std::optional<uint64_t>(0));
    EXPECT_EQ(catalog.readTokenCardinality(middle, kind, 1), std::optional<uint64_t>(1));
    EXPECT_EQ(metadata_->findIndexRoot(current, CANDIDATE_INDEX), root);
    EXPECT_EQ(metadata_->findPageLocation(current, root.root_page_id), location);
    EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, created)); EXPECT_TRUE(metadata_->findNodePageLocationOptional(middle, created));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, current, CANDIDATE_INDEX, 128, 8192).edges, 90U);
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(CoveredAdjacencyCandidateTest, NodeOnlyFinalizationRefusesAnOrdinaryScalarRootAndAnExceededEntityBound) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    constexpr uint64_t ordinary = CANDIDATE_INDEX + 9;
    {
        auto tx = planner.beginTransaction(); const std::array<BTreePageEntry, 1> entries{{{{std::byte{1}}, 7}}};
        tx.stageScalarIndexCreate(*btree_pages_, ordinary, entries); ASSERT_TRUE(tx.commit().committed());
    }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); (void)tx.createNode({});
        const auto before = metadata_->openSnapshot().superblock(); const auto epoch = io_->dataMutationEpoch();
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(ordinary, 1, memory), FormatException);
        EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback();
        EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    }
    {
        auto tx = planner.beginTransaction(); (void)tx.createNode({}); (void)tx.createNode({});
        EXPECT_THROW(tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory), FormatException);
        EXPECT_THROW((void)tx.commit(), FormatException); tx.rollback(); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(CoveredAdjacencyCandidateTest, LastEdgeRemovalRetainsAnEmptyCandidateRootAndRecreationKeepsOldPins) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
        session.setGraphCardinalityStatistics({2, 90});
        for (uint64_t type : {0U, 1U, 2U}) session.setTokenCardinality(kind, type, 30);
        session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    auto old = metadata_->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    for (size_t first = 0; first < 89;) {
        const auto count = (std::min)(size_t{64}, 89U - first); auto tx = planner.beginTransaction();
        for (size_t i = first; i < first + count; ++i) tx.eraseEdge(edges[i].entity_id);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, count, memory, nullptr, nullptr, &catalog);
        ASSERT_TRUE(tx.commit().committed()); first += count; EXPECT_EQ(memory->used(), 0U);
    }
    auto last = metadata_->openSnapshot();
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, last, CANDIDATE_INDEX, 128, 8192).edges, 1U);
    {
        auto tx = planner.beginTransaction(); tx.eraseEdge(edges.back().entity_id);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
    }
    auto empty = metadata_->openSnapshot(); EXPECT_TRUE(metadata_->findIndexRootOptional(empty, CANDIDATE_INDEX));
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, empty, CANDIDATE_INDEX, 128, 8192).edges, 0U);
    const auto emptyPage = Candidate::scan(*btree_pages_, *entities_, *metadata_, empty, CANDIDATE_INDEX,
        GlobalAdjacencyDirection::GAD_OUT, source, 64);
    EXPECT_TRUE(emptyPage.topology.entries().empty());
    EXPECT_EQ(catalog.readGraphCardinalityStatistics(empty), (std::optional<NativeGraphCardinalityStatistics>{{2, 0}}));
    for (uint64_t type : {0U, 1U, 2U}) EXPECT_EQ(catalog.readTokenCardinality(empty, kind, type), std::optional<uint64_t>(0));
    uint64_t created = 0;
    {
        auto tx = planner.beginTransaction(); created = tx.createEdge(target, source, 0);
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 1, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
    }
    EXPECT_GT(created, edges.back().entity_id); const auto current = metadata_->openSnapshot();
    EXPECT_EQ(scanAll(current, GlobalAdjacencyDirection::GAD_OUT, target), (std::vector<uint64_t>{created}));
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source).size(), 90U);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, empty, CANDIDATE_INDEX, 128, 8192).edges, 0U);
    EXPECT_EQ(catalog.readTokenCardinality(current, kind, 0), std::optional<uint64_t>(1));
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(CoveredAdjacencyCandidateTest, DeterministicSkewedInsertDeleteCyclesKeepEveryDirectionAndCompleteTypeCount) {
    seed(); NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    SystemCatalog catalog(*btree_pages_, *metadata_); const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
    {
        auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx); session.setGraphCardinalityStatistics({2, 90});
        for (uint64_t type : {0U, 1U, 2U}) session.setTokenCardinality(kind, type, 30);
        session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed());
    }
    std::vector<uint64_t> nodes{source, target};
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024 * 1024);
    {
        auto tx = planner.beginTransaction(); for (size_t i = 0; i < 32; ++i) nodes.push_back(tx.createNode({}));
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 32, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
    }
    std::map<uint64_t, EntityRef> expected;
    for (const auto &edge : edges) expected.emplace(edge.entity_id, edge);
    auto old = metadata_->openSnapshot(); uint32_t random = 0x5a17U;
    const auto next = [&] { random = random * 1664525U + 1013904223U; return random; };
    for (size_t round = 0; round < 4; ++round) {
        auto tx = planner.beginTransaction();
        for (size_t i = 0; i < 5; ++i) {
            auto selected = expected.begin(); std::advance(selected, next() % expected.size());
            tx.eraseEdge(selected->first); expected.erase(selected);
        }
        for (size_t i = 0; i < 7; ++i) {
            const auto from = i % 4U == 0U ? source : nodes[next() % nodes.size()];
            const auto to = i % 5U == 0U ? from : nodes[next() % nodes.size()];
            const uint64_t type = i % 4U == 3U ? 7U : i % 4U;
            const auto id = tx.createEdge(from, to, type); expected.emplace(id, EntityRef{id, true, from, to, type});
        }
        tx.stageCoveredAdjacencyCandidate(CANDIDATE_INDEX, 12, memory, nullptr, nullptr, &catalog); ASSERT_TRUE(tx.commit().committed());
        const auto current = metadata_->openSnapshot();
        const auto report = Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, current, CANDIDATE_INDEX, 256, 8192);
        EXPECT_EQ(report.edges, expected.size()); EXPECT_EQ(report.candidate_entries, expected.size() * 2U);
        std::array<uint64_t, 8> counts{};
        for (const auto &[id, edge] : expected) { static_cast<void>(id); ++counts[edge.type_token]; }
        for (uint64_t type : {0U, 1U, 2U, 7U}) EXPECT_EQ(catalog.readTokenCardinality(current, kind, type), std::optional(counts[type]));
        EXPECT_EQ(catalog.readGraphCardinalityStatistics(current), (std::optional<NativeGraphCardinalityStatistics>{{nodes.size(), expected.size()}}));
        for (const auto node : nodes) for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
            std::vector<uint64_t> ids;
            for (const auto &[id, edge] : expected)
                if ((direction == GlobalAdjacencyDirection::GAD_OUT ? edge.source_node_id : edge.target_node_id) == node) ids.push_back(id);
            EXPECT_EQ(scanAll(current, direction, node), ids);
        }
        EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, old, CANDIDATE_INDEX, 256, 8192).edges, 90U);
        EXPECT_EQ(memory->used(), 0U);
    }
}

TEST_F(CoveredAdjacencyCandidateTest, LogicalCoordinatorMaintainsCandidateAndRollsBackOverBudgetBatch) {
    seed(); auto old = metadata_->openSnapshot();
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
    auto executor = logicalExecutor(planner, graph, logicalLimits());
    LogicalGraphMutationBatch erase;
    erase.edge_deletes = {{edges[0].entity_id}, {edges[89].entity_id}};
    ASSERT_TRUE(executor.execute(erase).committed());
    auto after = metadata_->openSnapshot();
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, after, CANDIDATE_INDEX, 100, 64).edges, 88U);
    EXPECT_EQ(scanAll(old, GlobalAdjacencyDirection::GAD_OUT, source).size(), 90U);
    LogicalGraphMutationBatch tooLarge;
    tooLarge.edge_deletes = {{edges[1].entity_id}, {edges[2].entity_id}, {edges[3].entity_id}};
    EXPECT_THROW(executor.execute(tooLarge), FormatException);
    auto unchanged = metadata_->openSnapshot();
    EXPECT_EQ(unchanged.superblock().generation, after.superblock().generation);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, unchanged, CANDIDATE_INDEX, 100, 64).edges, 88U);
    // A non-topology publication still authenticates and preserves the root.
    LogicalGraphMutationBatch labels;
    labels.node_label_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, target}, {"Label"}}};
    ASSERT_TRUE(executor.execute(labels).committed());
    auto labeled = metadata_->openSnapshot();
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, labeled, CANDIDATE_INDEX, 100, 64).edges, 88U);

    const LogicalGraphMutationEntityReference from{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, source};
    const LogicalGraphMutationEntityReference to{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, target};
    LogicalGraphMutationBatch create;
    create.edge_creates = {{from, to, "Type", {}}, {from, from, "Type", {}}};
    ASSERT_TRUE(executor.execute(create).committed());
    auto inserted = metadata_->openSnapshot();
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, inserted, CANDIDATE_INDEX, 100, 64).edges, 90U);
    EXPECT_EQ(scanAll(inserted, GlobalAdjacencyDirection::GAD_IN, source).size(), 1U);
    LogicalGraphMutationBatch properties;
    properties.property_replacements = {{from, {{"value", graph::PropertyValue(int64_t(7))}}}};
    ASSERT_TRUE(executor.execute(properties).committed());
    auto propertyVersion = metadata_->openSnapshot();
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, propertyVersion, CANDIDATE_INDEX, 100, 64).edges, 90U);
    LogicalGraphMutationBatch detach;
    detach.node_deletes = {{source, true, 100U}};
    EXPECT_THROW(executor.execute(detach), FormatException);
    auto refused = metadata_->openSnapshot();
    EXPECT_EQ(refused.superblock().generation, propertyVersion.superblock().generation);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, refused, CANDIDATE_INDEX, 100, 64).edges, 90U);
}

TEST_F(CoveredAdjacencyCandidateTest, LogicalCoordinatorRejectsInvalidSelectionBeforePublishing) {
    seed();
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options());
    PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
    auto before = metadata_->openSnapshot();
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto limits = logicalLimits();
        if (mode == 0) limits.covered_adjacency_candidate_index = uint64_t(1) << 62;
        if (mode == 1) limits.maximum_covered_adjacency_changes = 0;
        if (mode == 2) limits.maximum_covered_adjacency_changes = 65;
        if (mode == 3) limits.covered_adjacency_workspace_bytes = 0;
        if (mode == 4) limits.covered_adjacency_workspace_bytes = 1024 * 1024 + 1;
        if (mode == 5) limits.covered_adjacency_candidate_index = 0;
        EXPECT_THROW(logicalExecutor(planner, graph, limits), std::invalid_argument) << mode;
    }
    auto limits = logicalLimits(); ++limits.covered_adjacency_candidate_index;
    auto missingRoot = logicalExecutor(planner, graph, limits);
    LogicalGraphMutationBatch erase; erase.edge_deletes = {{edges[0].entity_id}};
    EXPECT_THROW(missingRoot.execute(erase), FormatException);
    limits = logicalLimits(); limits.covered_adjacency_workspace_bytes = 1;
    auto noWorkspace = logicalExecutor(planner, graph, limits);
    EXPECT_ANY_THROW(noWorkspace.execute(erase));
    auto after = metadata_->openSnapshot();
    EXPECT_EQ(after.superblock().generation, before.superblock().generation);
    EXPECT_EQ(Candidate::verifyIntegrity(*btree_pages_, *entities_, *metadata_, after, CANDIDATE_INDEX, 100, 64).edges, 90U);
}
