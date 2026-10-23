#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyLayoutScaleTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    void inspectRetirementFault(bool completeHeader) {
        seed(NODE, 64, 2);
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        // Seed one explicitly retired, otherwise unreferenced Catalog segment.
        // A small ROW seed need not retire an entire pool during conversion.
        const auto initial = metadata_->openSnapshot().superblock();
        const CatalogSlotReservationRequest reserved{SegmentId(initial.next_logical_segment_id),
            SegmentKind::SGK_PROPERTY_PAGE_POOL, 999, Generation(1), SegmentState::SGS_SEALED, true};
        const auto allocated = metadata_->reserveFirstFreeAndUpsertCatalog(std::span(&reserved, 1),
            PhysicalSlotId(40), PhysicalSlotId(80), LogSequenceNumber(initial.checkpoint_lsn.value() + 1));
        ASSERT_TRUE(allocated);
        (void)metadata_->retireCatalogSegment(reserved.segment_id, Generation(1),
            LogSequenceNumber(allocated->superblock.checkpoint_lsn.value() + 1));
        auto old = metadata_->openSnapshot();
        const auto expectedFirst = properties_->readEntity(old, {NODE, 1});
        const auto expectedLast = properties_->readEntity(old, {NODE, 64});
        auto conversion = request(NODE, memory);
        conversion.conversion->compact_column_memberships = true;
        conversion.conversion->publish_candidate = true; run(conversion);
        const auto active = metadata_->openSnapshot().superblock();
        EXPECT_THROW((void)metadata_->allocateLogicalPageIds(1, LogSequenceNumber(active.checkpoint_lsn.value() + 1)), FormatException);
        // No active-pin authority may be discarded to make room for a pass.
        EXPECT_EQ(properties_->readEntity(old, {NODE, 1}), expectedFirst);
        const auto epoch = io_->dataMutationEpoch();
        EXPECT_EQ(metadata_->reclaimRetiredPhysicalSlots(LogSequenceNumber(active.checkpoint_lsn.value() + 1)).reclaimed_physical_slots, 0U);
        EXPECT_EQ(io_->dataMutationEpoch(), epoch);
        EXPECT_EQ(metadata_->openSnapshot().superblock(), active);
        old.release();
        ASSERT_EQ(metadata_->activeSnapshotCount(), 0U);
        size_t queued = 0;
        { auto snapshot = metadata_->openSnapshot();
          metadata_->visitRetireQueueEntries(snapshot, [&](const RetireQueueEntry &) { ++queued; }); }
        ASSERT_GT(queued, 0U);
        MetadataPublishOptions failure;
        failure.bootstrap_failpoint = completeHeader ? BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE :
            BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE;
        EXPECT_THROW((void)metadata_->reclaimRetiredPhysicalSlots(
            LogSequenceNumber(active.checkpoint_lsn.value() + 1), failure), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io_);
        auto pin = reopened->openSnapshot();
        EXPECT_EQ(pin.superblock().generation.value(), active.generation.value() + (completeHeader ? 1U : 0U));
        EXPECT_EQ(pin.superblock().hybrid_property_descriptor_root, active.hybrid_property_descriptor_root);
        SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 1}), expectedFirst);
        EXPECT_EQ(roots.readEntity(pin, {NODE, 64}), expectedLast);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *reopened, memory).scan());
        EXPECT_EQ(memory->used(), 0U);
    }
    static NativeGraphWritePlannerOptions placement() {
        NativeGraphWritePlannerOptions result;
        result.first_slot = PhysicalSlotId(40); result.last_slot = PhysicalSlotId(80);
        result.entity_pool_owner_id = 901; result.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        result.logical_id_lease_block_size = 1;
        return result;
    }
    static PropertyCollectionRoot expected(uint64_t id) {
        return {id, graph::Node::typeId, {{1, PropertyValue(id % 2 ? "US" : "DE")}, {2, PropertyValue(int64_t(id))}}};
    }
    void inspect(size_t owners, bool keepOldPin) {
        NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, placement());
        for (uint64_t first = 1; first <= owners; first += 64) {
            auto tx = writer.beginTransaction(); tx.reserveNodeIds(64);
            for (uint64_t id = first; id < first + 64; ++id) {
                EXPECT_EQ(tx.createNodeWithReservedId(id, {}), id);
                tx.replaceNodeProperty(expected(id));
            }
            ASSERT_TRUE(tx.commit().committed());
        }
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        NativeStorageAllocationCensus census(io_, *metadata_, memory);
        const auto record = [&](const std::string &prefix) {
            const auto cost = census.inspect(metadata_->openSnapshot());
            if (!cost || !cost->fullyAttributed()) throw std::runtime_error("incomplete scale census");
            RecordProperty(prefix + "_steady", std::to_string(cost->steadyAllocatedBytes()));
            RecordProperty(prefix + "_total", std::to_string(cost->totalAllocatedBytes()));
            RecordProperty(prefix + "_file", std::to_string(cost->file_bytes));
            RecordProperty(prefix + "_property", std::to_string(cost->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_PROPERTY)]));
            RecordProperty(prefix + "_maps", std::to_string(cost->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_HYBRID_MAP)]));
            RecordProperty(prefix + "_metadata", std::to_string(cost->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_METADATA)]));
            EXPECT_EQ(memory->used(), 0U);
        };
        record("row");
        auto old = metadata_->openSnapshot();
        std::vector<PhysicalSlotId> sourcePropertySlots;
        metadata_->visitCatalogEntries(old, [&](const CatalogEntry &entry) {
            if (entry.state == SegmentState::SGS_SEALED && entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL)
                sourcePropertySlots.push_back(entry.physical_slot);
        });
        if (!keepOldPin) old.release();
        // Explicit isolated-format laboratory only: no allocation-admission
        // capability is claimed by these ungated one-group publications.
        // This census decides whether mature amortization merits a production
        // migration design; it does not authorize exposing that migration.
        size_t completed = 0;
        for (uint64_t group = 1; group <= owners / 64; ++group) {
            auto conversion = request(NODE, memory);
            conversion.conversion->group_id = group;
            conversion.conversion->compact_column_memberships = true;
            conversion.conversion->publish_candidate = true;
            const auto before = metadata_->openSnapshot().superblock();
            try { run(conversion); }
            catch (const FormatException &error) {
                // A retained source pin can exhaust this fixture's existing
                // finite destination range. Refusal must remain atomic; never
                // enlarge the range to hide that peak-space requirement.
                ASSERT_TRUE(keepOldPin) << error.what();
                ASSERT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
                ASSERT_NE(std::string(error.what()).find("native placement has no free slot"), std::string::npos);
                EXPECT_EQ(metadata_->openSnapshot().superblock(), before);
                EXPECT_EQ(memory->used(), 0U);
                RecordProperty("capacity_refusal_group", std::to_string(group));
                break;
            }
            completed = group; EXPECT_EQ(memory->used(), 0U);
            const auto state = metadata_->openSnapshot().superblock();
            (void)metadata_->reclaimRetiredPhysicalSlots(LogSequenceNumber(state.checkpoint_lsn.value() + 1));
            const auto afterReclaim = metadata_->openSnapshot();
            EXPECT_EQ(afterReclaim.superblock().hybrid_property_descriptor_root, state.hybrid_property_descriptor_root);
            EXPECT_EQ(afterReclaim.superblock().next_logical_node_id, state.next_logical_node_id);
            if (keepOldPin)
                for (const auto slot : sourcePropertySlots)
                    EXPECT_TRUE(metadata_->isPhysicalSlotAllocated(afterReclaim, slot));
            if (group == 8 || group == owners / 64) record("groups_" + std::to_string(group));
        }
        RecordProperty("completed_groups", std::to_string(completed));
        if (!keepOldPin) EXPECT_EQ(completed, owners / 64);
        record("final");
        auto current = metadata_->openSnapshot();
        for (uint64_t id = 1; id <= owners; ++id) {
            EXPECT_EQ(properties_->readEntity(current, {NODE, id}), expected(id));
            if (keepOldPin) EXPECT_EQ(properties_->readEntity(old, {NODE, id}), expected(id));
        }
        // Full integrity uses the existing maintenance grant, independently
        // from the unchanged <=1 MiB conversion/census account.
        auto integrityMemory = std::make_shared<MaterializedByteBudget>(
            RelocatableStorageBackendOptions{}.maximum_reclaim_workspace_bytes);
        EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, integrityMemory).scan());
        EXPECT_EQ(integrityMemory->used(), 0U);
        old.release(); current.release();
        record("unpinned");
        auto reopened = RelocatableMetadataStore::openReadOnly(io_);
        SlottedPagePool pages(io_, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore roots(pages, *reopened);
        auto pin = reopened->openSnapshot();
        for (uint64_t id : {uint64_t(1), uint64_t(owners)})
            EXPECT_EQ(roots.readEntity(pin, {NODE, id}), expected(id));
        EXPECT_EQ(memory->used(), 0U);
    }
};
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyLayoutScaleTest, IsolatedPinnedNarrowLayoutCensusAt512Owners) { inspect(512, true); }
TEST_F(HybridPropertyLayoutScaleTest, IsolatedPinnedNarrowLayoutCensusAt4096Owners) { inspect(4096, true); }
TEST_F(HybridPropertyLayoutScaleTest, IsolatedDrainedNarrowLayoutCensusAt4096Owners) { inspect(4096, false); }
TEST_F(HybridPropertyLayoutScaleTest, PartialRetirementPublicationRetainsOldAllocationAndNativeValues) { inspectRetirementFault(false); }
TEST_F(HybridPropertyLayoutScaleTest, CompleteRetirementPublicationRecoversNewAllocationAndNativeValues) { inspectRetirementFault(true); }
#endif
