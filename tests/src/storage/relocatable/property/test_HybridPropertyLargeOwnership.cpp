#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::OwnedPropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
struct OwnershipSink final : LargeValueOwnershipScanSink {
    std::vector<LargeValueId> ids;
    void markReachable(const LargeValueRef &ref) override { ids.push_back(ref.value_id); }
};
class HybridPropertyLargeOwnershipTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902}; return options;
    }
    void seedMixed(LargeValueStore &values, PropertyCollectionRootStore &roots, const std::shared_ptr<MaterializedByteBudget> &memory) {
        seed(NODE, 65, 0);
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, placement());
        auto tx = planner.beginTransaction(); tx.replaceNodeProperty({65, graph::Node::typeId, {{1, PropertyValue(std::string(8192, 'r'))}}});
        const auto edge = tx.createEdge(1, 2, 7); tx.replaceEdgeProperty({edge, graph::Edge::typeId, {{1, PropertyValue(std::string(8192, 'e'))}}});
        ASSERT_TRUE(tx.commit().committed());
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; ASSERT_NO_THROW(run(conversion));
        const auto state = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
            {{NODE, 1}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'x')))},
            {{NODE, 1}, 2, OwnedPropertyValue(PropertyValue(std::string(8192, 'y')))},
            {{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'z')))}}};
        NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
            cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    }
};
}
TEST_F(HybridPropertyLargeOwnershipTest, RowScannerRetainsItsOriginalRecordCursorInBothFormats) {
    seed(NODE, 2, 0); const auto pin = metadata_->openSnapshot(); OwnershipSink sink;
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_);
    const auto result = scanner.scanReachableAfter(pin, 0, 1, 128, sink);
    EXPECT_TRUE(result.exhausted); EXPECT_EQ(result.locations_scanned, 0U); EXPECT_TRUE(sink.ids.empty());
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLargeOwnershipTest, MixedNativeAndRowOwnersAreScannedInOneStableLogicalOrder) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_); OwnershipSink all;
    const auto complete = scanner.scanReachableAfter(pin, 0, 64, 4096, all);
    EXPECT_TRUE(complete.exhausted); EXPECT_EQ(complete.locations_scanned, 5U); ASSERT_EQ(all.ids.size(), 5U);
    EXPECT_EQ(complete.last_scanned_location_id, (EntityPropertyKey{EDGE, 1}.directoryId()));
    EXPECT_GT(complete.metadata_pages_scanned, 0U); EXPECT_LE(complete.metadata_pages_scanned, 4096U);
    OwnershipSink tail; const auto afterFirst = scanner.scanReachableAfter(pin, EntityPropertyKey{NODE, 1}.directoryId(), 64, 4096, tail);
    EXPECT_TRUE(afterFirst.exhausted); EXPECT_EQ(afterFirst.locations_scanned, 3U);
    EXPECT_EQ(tail.ids, std::vector<LargeValueId>(all.ids.begin() + 2, all.ids.end()));
    OwnershipSink edge; const auto afterRows = scanner.scanReachableAfter(pin, EntityPropertyKey{NODE, 65}.directoryId(), 64, 4096, edge);
    EXPECT_TRUE(afterRows.exhausted); EXPECT_EQ(afterRows.locations_scanned, 1U); ASSERT_EQ(edge.ids.size(), 1U); EXPECT_EQ(edge.ids[0], all.ids.back());
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeOwnershipTest, OwnerAndDirectoryBudgetsNeverClaimAnIncompleteNativeScanIsExhausted) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    const auto pin = metadata_->openSnapshot(); const auto before = pin.superblock(); const auto epoch = io_->dataMutationEpoch();
    EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_); OwnershipSink partial;
    EXPECT_THROW((void)scanner.scanReachableAfter(pin, 0, 1, 4096, partial), FormatException);
    OwnershipSink noPages; const auto absent = scanner.scanReachableAfter(pin, 0, 64, 0, noPages);
    EXPECT_FALSE(absent.exhausted); EXPECT_EQ(absent.last_scanned_location_id, 0U); EXPECT_EQ(absent.metadata_pages_scanned, 0U); EXPECT_TRUE(noPages.ids.empty());
    OwnershipSink onePage; const auto capped = scanner.scanReachableAfter(pin, 0, 64, 1, onePage);
    EXPECT_FALSE(capped.exhausted); EXPECT_LE(capped.metadata_pages_scanned, 1U); EXPECT_EQ(capped.last_scanned_location_id, 0U);
    OwnershipSink retry; const auto complete = scanner.scanReachableAfter(pin, 0, 64, 4096, retry);
    EXPECT_TRUE(complete.exhausted); EXPECT_EQ(retry.ids.size(), 5U);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeOwnershipTest, LogicalOwnerCursorCanResumeOnlyAfterAllMembershipsOfOneOwner) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    const auto pin = metadata_->openSnapshot(); EntityPropertyLargeValueOwnershipScanner scanner(*slotted_pages_, *metadata_);
    OwnershipSink first; const auto partial = scanner.scanReachableAfter(pin, 0, 2, 4096, first);
    EXPECT_FALSE(partial.exhausted); EXPECT_EQ(partial.locations_scanned, 2U); EXPECT_EQ(partial.last_scanned_location_id, 1U); ASSERT_EQ(first.ids.size(), 2U);
    OwnershipSink rest; const auto complete = scanner.scanReachableAfter(pin, partial.last_scanned_location_id, 64, 4096, rest);
    EXPECT_TRUE(complete.exhausted); EXPECT_EQ(rest.ids.size(), 3U); EXPECT_EQ(complete.locations_scanned, 3U);
    HybridPropertyReadContext context(*slotted_pages_, *metadata_, pin);
    EXPECT_EQ(context.nextOwnerAfter(NODE, 0), std::optional(EntityPropertyKey{NODE, 1}));
    EXPECT_EQ(context.nextOwnerAfter(NODE, 1), std::optional(EntityPropertyKey{NODE, 2}));
    EXPECT_FALSE(context.nextOwnerAfter(NODE, 2)); EXPECT_FALSE(context.nextOwnerAfter(NODE, RSS_HYBRID_MAX_ENTITY_ID));
    EXPECT_FALSE(context.nextOwnerAfter(EDGE, 0)); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeOwnershipTest, CompleteNativeOwnershipProofRetiresOnlyOrphansAndPreservesPinnedValues) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    auto old = metadata_->openSnapshot(); const auto previous = roots.readEntity(old, {NODE, 2});
    const auto state = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
        {{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'w')))}}};
    NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot(); const auto expected = roots.readEntity(current, {NODE, 2});
    const auto descriptor = current.superblock().hybrid_property_descriptor_root;
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 4096; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    const auto before = current.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto incomplete = options; incomplete.maximum_owner_metadata_pages = 1;
    EXPECT_THROW((void)reclaimer.reclaim(incomplete, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    OwnershipSink sourceRefs; EntityPropertyLargeValueOwnershipScanner sourceScanner(*slotted_pages_, *metadata_);
    ASSERT_TRUE(sourceScanner.scanReachableAfter(old, 0, 64, 4096, sourceRefs).exhausted);
    ASSERT_EQ(sourceRefs.ids.size(), 5U); const auto orphanId = sourceRefs.ids[2];
    std::vector<PageRetirement> unprovenRetirements;
    for (const auto &manifest : values.manifestTree(current, values.manifest(current, orphanId))) {
        unprovenRetirements.push_back(values.planPageRetirement(current, manifest.page_id, orphanId, PageOwnerKind::POK_LARGE_VALUE_MANIFEST));
        for (const auto &chunk : manifest.chunks) for (uint16_t ordinal = 0; ordinal < chunk.data_page_count; ++ordinal)
            unprovenRetirements.push_back(values.planPageRetirement(current, PageId(chunk.first_data_page_id.value() + ordinal), orphanId, PageOwnerKind::POK_LARGE_VALUE_DATA));
    }
    EXPECT_THROW((void)values.retireUnreachablePages(before.generation, std::span(&orphanId, 1), unprovenRetirements,
        PhysicalSlotId(40), PhysicalSlotId(80), LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const auto report = reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    EXPECT_EQ(report.reachable_values, 5U); EXPECT_EQ(report.retired_values, 1U);
    EXPECT_EQ(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root, descriptor);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 2}), expected);
    EXPECT_EQ(roots.readEntity(old, {NODE, 2}), previous);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 65}), roots.readEntity(old, {NODE, 65}));
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {EDGE, 1}), roots.readEntity(old, {EDGE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    old.release(); current.release(); const auto after = metadata_->openSnapshot().superblock();
    const auto cleanup = reclaimer.reclaim(options, LogSequenceNumber(after.checkpoint_lsn.value() + 1));
    EXPECT_EQ(cleanup.retired_values, 0U);
    EXPECT_EQ(cleanup.reachable_values, 5U); EXPECT_EQ(cleanup.retired_manifest_pages, 0U);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 2}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeOwnershipTest, NativeReclamationRecoverySelectsCompleteOldOrNewRoutes) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    auto old = metadata_->openSnapshot(); const auto previous = roots.readEntity(old, {NODE, 2});
    const auto state = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
        {{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'w')))}}};
    NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot(); const auto expected = roots.readEntity(current, {NODE, 2});
    const auto descriptor = current.superblock().hybrid_property_descriptor_root;
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 4096; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    for (const auto point : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE, BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        const auto before = metadata_->openSnapshot().superblock();
        MetadataPublishOptions interruption; interruption.bootstrap_failpoint = point;
        EXPECT_THROW((void)reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 1), interruption), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io_); auto pin = recovered->openSnapshot();
        SlottedPagePool pages(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore recoveredValues(io_, *recovered, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore recoveredRoots(pages, recoveredValues, *recovered);
        const bool installed = point == BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
        EXPECT_EQ(pin.superblock().generation.value(), before.generation.value() + size_t(installed));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, descriptor);
        EXPECT_EQ(recoveredRoots.readEntity(pin, {NODE, 2}), expected);
        EXPECT_EQ(roots.readEntity(old, {NODE, 2}), previous);
        size_t manifests = 0; recovered->visitLargeValueManifestLocations(pin, [&](const LargeValueManifestLocation &) { ++manifests; });
        EXPECT_EQ(manifests, installed ? 5U : 6U);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *recovered, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    }
}
TEST_F(HybridPropertyLargeOwnershipTest, RemovingAllNativeLargeFieldsReleasesOnlyUnpinnedImmutablePools) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    auto old = metadata_->openSnapshot(); const auto previous = roots.readEntity(old, {NODE, 2});
    const auto state = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 3> cells{{
        {{NODE, 1}, 1, {}, true}, {{NODE, 1}, 2, {}, true}, {{NODE, 2}, 1, {}, true}}};
    NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot(); const auto expected = roots.readEntityOptional(current, {NODE, 2});
    const auto descriptor = current.superblock().hybrid_property_descriptor_root;
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 4096; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    const auto before = current.superblock(); const auto epoch = io_->dataMutationEpoch();
    auto incomplete = options; incomplete.maximum_owner_metadata_pages = 1;
    EXPECT_THROW((void)reclaimer.reclaim(incomplete, LogSequenceNumber(before.checkpoint_lsn.value() + 1)), FormatException);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    const auto report = reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 1));
    EXPECT_EQ(report.reachable_values, 2U); EXPECT_EQ(report.retired_values, 3U);
    EXPECT_EQ(metadata_->openSnapshot().superblock().hybrid_property_descriptor_root, descriptor);
    EXPECT_EQ(roots.readEntityOptional(metadata_->openSnapshot(), {NODE, 2}), expected);
    EXPECT_EQ(roots.readEntity(old, {NODE, 2}), previous);
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {NODE, 65}), roots.readEntity(old, {NODE, 65}));
    EXPECT_EQ(roots.readEntity(metadata_->openSnapshot(), {EDGE, 1}), roots.readEntity(old, {EDGE, 1}));
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
    old.release(); current.release(); const auto after = metadata_->openSnapshot().superblock();
    const auto cleanup = reclaimer.reclaim(options, LogSequenceNumber(after.checkpoint_lsn.value() + 1));
    EXPECT_EQ(cleanup.retired_values, 0U);
    EXPECT_GT(cleanup.retired_pools_released, 0U);
    EXPECT_EQ(roots.readEntityOptional(metadata_->openSnapshot(), {NODE, 2}), expected);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyLargeOwnershipTest, ConcurrentNativePublicationInvalidatesTheWholeOwnershipProof) {
    LargeValueStore values(io_, *metadata_); PropertyCollectionRootStore roots(*slotted_pages_, values, *metadata_);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); seedMixed(values, roots, memory);
    auto old = metadata_->openSnapshot(); const auto previous = roots.readEntity(old, {NODE, 2});
    const auto state = old.superblock();
    const std::array<NativeHybridPropertyColumnReplacement, 1> cells{{
        {{NODE, 2}, 1, OwnedPropertyValue(PropertyValue(std::string(8192, 'w')))}}};
    NativeHybridPropertyColumnWrite write{state.database_id, state.generation, state.hybrid_property_descriptor_root,
        cells, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
    ASSERT_NO_THROW((void)metadata_->mutateHybridPropertyColumns(write, LogSequenceNumber(state.checkpoint_lsn.value() + 1)));
    auto current = metadata_->openSnapshot(); const auto expected = roots.readEntity(current, {NODE, 2});
    const auto descriptor = current.superblock().hybrid_property_descriptor_root;
    struct ConcurrentScanner final : LargeValueOwnershipScanner {
        std::function<void()> publish;
        std::string_view mediaType() const noexcept override { return "zz/test-gc-fence"; }
        Progress scanReachableAfter(const MetadataSnapshotPin &, uint64_t, size_t, size_t, LargeValueOwnershipScanSink &) const override {
            publish(); return {0, 0, 0, true};
        }
    };
    auto concurrent = std::make_shared<ConcurrentScanner>(); Superblock changed; uint64_t changedEpoch = 0;
    concurrent->publish = [&] {
        const auto source = metadata_->openSnapshot().superblock();
        const std::array<NativeHybridPropertyColumnReplacement, 1> update{{{{NODE, 2}, 3, int64_t(77)}}};
        NativeHybridPropertyColumnWrite mutation{source.database_id, source.generation, source.hybrid_property_descriptor_root,
            update, memory, PhysicalSlotId(40), PhysicalSlotId(80), 903, nullptr, true};
        changed = metadata_->mutateHybridPropertyColumns(mutation, LogSequenceNumber(source.checkpoint_lsn.value() + 1));
        changedEpoch = io_->dataMutationEpoch();
    };
    const std::array<LargeValueOwnershipScannerPtr, 1> additional{{concurrent}};
    LargeValueReachabilityReclaimer reclaimer(values, *slotted_pages_, *metadata_, additional);
    LargeValueReachabilityReclaimOptions options;
    options.maximum_manifest_locations = 32; options.maximum_manifest_metadata_pages = 128; options.maximum_manifest_nodes = 128;
    options.maximum_owner_locations = 128; options.maximum_owner_metadata_pages = 4096; options.maximum_data_pages = 128;
    options.maximum_retirements = 256; options.maximum_page_locations = 1024; options.maximum_pools = 64;
    options.first_index_slot = PhysicalSlotId(40); options.last_index_slot = PhysicalSlotId(80);
    const auto before = current.superblock();
    EXPECT_THROW((void)reclaimer.reclaim(options, LogSequenceNumber(before.checkpoint_lsn.value() + 2)), FormatException);
    EXPECT_GT(changed.generation, before.generation);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), changed); EXPECT_EQ(io_->dataMutationEpoch(), changedEpoch);
    EXPECT_EQ(roots.readEntity(old, {NODE, 2}), previous);
    size_t manifests = 0; metadata_->visitLargeValueManifestLocations(metadata_->openSnapshot(), [&](const LargeValueManifestLocation &) { ++manifests; });
    EXPECT_EQ(manifests, 6U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
#endif
