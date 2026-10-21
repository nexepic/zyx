#include "storage/relocatable/property/HybridPropertyOrdinaryConversionTestFixture.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "benchmarks/architecture-design/property-subblock/PropertySubblockCandidate.hpp"
#include <iostream>
using namespace graph::storage;
using graph::query::execution::MaterializedByteBudget;
namespace {
class HybridPropertyAllocationCostsTest : public graph::storage::test::HybridPropertyOrdinaryConversionTestFixture {
protected:
    auto census(const std::shared_ptr<MaterializedByteBudget> &memory) { return NativeStorageAllocationCensus(io_, *metadata_, memory); }
    void convert(const std::shared_ptr<MaterializedByteBudget> &memory) {
        auto conversion = request(NODE, memory); conversion.conversion->publish_candidate = true; run(conversion);
    }
};
class CostCancelled final : public CancellationCheckpoint {
    void check() const override { throw std::runtime_error("allocation census cancelled"); }
};
}
TEST_F(HybridPropertyAllocationCostsTest, CountsWholePoolsAndTheNonCatalogEmergencyReserveWithoutWriting) {
    seed(NODE, 64, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto pin = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch(); const auto bytes = io_->fileSize();
    auto footprint = census(memory).inspect(pin); ASSERT_TRUE(footprint); EXPECT_TRUE(footprint->fullyAttributed());
    EXPECT_EQ(footprint->bootstrap_bytes, pin.superblock().data_region_offset);
    EXPECT_EQ(footprint->emergency_reserve_bytes, RSS_SEGMENT_SIZE);
    EXPECT_EQ(footprint->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_PROPERTY)], RSS_SEGMENT_SIZE);
    uint64_t catalogBytes = 0; metadata_->visitCatalogEntries(pin, [&](const CatalogEntry &entry) {
        if (entry.state == SegmentState::SGS_SEALED) catalogBytes += RSS_SEGMENT_SIZE;
    });
    EXPECT_EQ(footprint->totalAllocatedBytes(), catalogBytes + RSS_SEGMENT_SIZE + footprint->bootstrap_bytes);
    EXPECT_EQ(footprint->totalAllocatedBytes(), footprint->steadyAllocatedBytes());
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(io_->fileSize(), bytes); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationCostsTest, EnumeratesUnattributedAllocationBitsBeyondThePhysicalFileAndAcrossRadixLevels) {
    const PhysicalSlotId far(uint64_t(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY) * RSS_CATALOG_RADIX_FANOUT + 17U);
    auto previous = metadata_->openSnapshot(); const auto state = previous.superblock();
    ASSERT_NO_THROW((void)metadata_->setPhysicalSlotAllocated(far, true, LogSequenceNumber(state.checkpoint_lsn.value() + 1U)));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto pin = metadata_->openSnapshot();
    EXPECT_GT(far.value(), (io_->fileSize() - pin.superblock().data_region_offset) / RSS_SEGMENT_SIZE);
    const auto epoch = io_->dataMutationEpoch(); const auto footprint = census(memory).inspect(pin);
    ASSERT_TRUE(footprint); EXPECT_FALSE(footprint->fullyAttributed()); EXPECT_EQ(footprint->unattributed_allocated_bytes, RSS_SEGMENT_SIZE);
    EXPECT_GE(footprint->allocator_metadata_pages, 4U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_THROW((void)census(memory).inspect(previous), FormatException); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationCostsTest, EveryExplicitWorkLimitReturnsUnknownAndQuotaOrCancellationNeverWrites) {
    seed(NODE, 64, 2); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto pin = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    for (int boundary = 0; boundary < 3; ++boundary) {
        NativeStorageAllocationCensusOptions options;
        if (boundary == 0) options.maximum_catalog_entries = 0;
        if (boundary == 1) options.maximum_allocated_slots = 1;
        if (boundary == 2) options.maximum_allocator_metadata_pages = 1;
        EXPECT_FALSE(census(memory).inspect(pin, options)); EXPECT_EQ(memory->used(), 0U);
    }
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW((void)census(tiny).inspect(pin), graph::query::execution::MaterializedByteLimitExceeded);
    CostCancelled cancellation; NativeStorageAllocationCensusOptions options; options.cancellation = &cancellation;
    EXPECT_THROW((void)census(memory).inspect(pin, options), std::runtime_error);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyAllocationCostsTest, AllocationOnlyRelocationSourcesAreChargedOnceOutsideTheCurrentCatalog) {
    auto state = metadata_->openSnapshot().superblock();
    (void)metadata_->setPhysicalSlotAllocated(PhysicalSlotId(70), true, LogSequenceNumber(state.checkpoint_lsn.value() + 1U));
    state = metadata_->openSnapshot().superblock();
    const CatalogEntry entry{SegmentId(state.next_logical_segment_id), PhysicalSlotId(70), SegmentKind::SGK_ENTITY_PAGE_POOL,
        155, Generation(1), SegmentState::SGS_SEALED, LogSequenceNumber(state.checkpoint_lsn.value() + 1U)};
    (void)metadata_->upsertCatalog(std::span(&entry, 1), entry.committed_lsn);
    auto old = metadata_->openSnapshot(); state = old.superblock();
    // This metadata-layer fixture reserves ownership only; it deliberately
    // makes no payload-integrity claim and has no Page Directory children.
    auto moved = metadata_->relocateCatalogSegment(entry.segment_id, entry.generation, PhysicalSlotId(71), PhysicalSlotId(71),
        LogSequenceNumber(state.checkpoint_lsn.value() + 1U), [](const CatalogEntry &, const CatalogEntry &) {});
    ASSERT_TRUE(moved); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto pin = metadata_->openSnapshot();
    const auto epoch = io_->dataMutationEpoch(); const auto cost = census(memory).inspect(pin);
    ASSERT_TRUE(cost); EXPECT_TRUE(cost->fullyAttributed()); EXPECT_EQ(cost->relocated_source_bytes, RSS_SEGMENT_SIZE);
    EXPECT_EQ(cost->retired_catalog_bytes, 0U); EXPECT_EQ(cost->totalAllocatedBytes(), cost->steadyAllocatedBytes() + RSS_SEGMENT_SIZE);
    NativeStorageAllocationCensusOptions limited; limited.maximum_retirement_records = 0;
    EXPECT_FALSE(census(memory).inspect(pin, limited)); EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyAllocationCostsTest, ACommittedPoolWithLostAllocatorOwnershipIsAnErrorRatherThanUnknownCost) {
    seed(NODE, 2, 1); auto source = metadata_->openSnapshot();
    const auto pool = metadata_->findPageLocation(source, metadata_->findNodePageLocation(source, 1).page_id).segment_id;
    const auto entry = metadata_->findCatalog(source, pool); const auto state = source.superblock();
    (void)metadata_->setPhysicalSlotAllocated(entry.physical_slot, false, LogSequenceNumber(state.checkpoint_lsn.value() + 1U));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024); auto pin = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    EXPECT_THROW((void)census(memory).inspect(pin), FormatException);
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyAllocationCostsTest, ActualRowAndHybridAllocationsIncludeAllMapsMetadataRetirementAndPinnedPeak) {
    seed(NODE, 64, 16); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    auto row = metadata_->openSnapshot(); const auto rowValues = properties_->readEntity(row, {NODE, 32});
    auto rowCost = census(memory).inspect(row); ASSERT_TRUE(rowCost); ASSERT_TRUE(rowCost->fullyAttributed());
    ASSERT_NO_THROW(convert(memory)); auto candidate = metadata_->openSnapshot(); const auto epoch = io_->dataMutationEpoch();
    auto candidateCost = census(memory).inspect(candidate); ASSERT_TRUE(candidateCost); ASSERT_TRUE(candidateCost->fullyAttributed());
    EXPECT_EQ(properties_->readEntity(candidate, {NODE, 32}), rowValues); EXPECT_EQ(properties_->readEntity(row, {NODE, 32}), rowValues);
    EXPECT_GT(candidateCost->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_HYBRID_MAP)], 0U);
    EXPECT_GT(candidateCost->sealed_pool_bytes[size_t(NativeStorageAllocationKind::NSAK_METADATA)], 0U);
    EXPECT_GT(candidateCost->retired_catalog_bytes, 0U);
    EXPECT_GE(candidateCost->totalAllocatedBytes(), candidateCost->steadyAllocatedBytes() + candidateCost->retired_catalog_bytes);
    using namespace zyx::benchmark::subblock;
    CostEvidence evidence; evidence.known.fill(true); evidence.row_lower[0] = rowCost->steadyAllocatedBytes();
    evidence.candidate_upper[0] = candidateCost->steadyAllocatedBytes(); evidence.peak_known = true;
    evidence.peak_allocated_upper = std::max({rowCost->totalAllocatedBytes(), candidateCost->totalAllocatedBytes(), candidateCost->file_bytes});
    const auto gate = conversionGate(evidence, {64, 1024, 1024 * 1024, 1024 * 1024, UINT64_MAX});
    const auto threshold = rowCost->steadyAllocatedBytes() / 10U * 9U + rowCost->steadyAllocatedBytes() % 10U * 9U / 10U;
    EXPECT_EQ(gate, candidateCost->steadyAllocatedBytes() <= threshold ? GateResult::HPG_ADMIT : GateResult::HPG_INSUFFICIENT_SAVING);
    std::cout << "NATIVE_WHOLE_POOL_COST row=" << rowCost->steadyAllocatedBytes() << " candidate=" << candidateCost->steadyAllocatedBytes()
        << " retained=" << candidateCost->retired_catalog_bytes << " allocated_peak=" << candidateCost->totalAllocatedBytes()
        << " file=" << candidateCost->file_bytes << '\n';
    EXPECT_EQ(io_->dataMutationEpoch(), epoch); EXPECT_EQ(memory->used(), 0U);
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan());
}
#endif
