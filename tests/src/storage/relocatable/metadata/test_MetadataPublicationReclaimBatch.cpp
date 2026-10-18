#include <array>
#include <gtest/gtest.h>
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;
namespace {
Superblock initialBlock() {
    Superblock s; s.database_id = DatabaseId(0x1020304050607080ULL);
    s.generation = Generation(1); s.checkpoint_lsn = LogSequenceNumber(1); return s;
}
void prepareHeadroom(MetadataPagePoolAllocator &allocator, const Superblock &s) {
    (void)allocator.allocatePages(s.generation, s.checkpoint_lsn, MetadataPagePoolAllocator::PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES + 2U * RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr);
    allocator.reclaimUnreachable(s, {});
}
CatalogEntry poolEntry(uint64_t slot, uint64_t generation) {
    CatalogEntry e; e.segment_id = SegmentId(1); e.physical_slot = PhysicalSlotId(slot);
    e.kind = SegmentKind::SGK_ENTITY_PAGE_POOL; e.owner_id = 19;
    e.generation = Generation(generation); e.state = SegmentState::SGS_SEALED;
    e.committed_lsn = LogSequenceNumber(generation); return e;
}
}
TEST_F(RelocatableMetadataPagePoolTest, DeferredPublicationRetainsFramesUntilBoundedAllocationThreshold) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    MetadataPagePoolAllocator allocator(io, s); prepareHeadroom(allocator, s);
    for (uint32_t i = 0; i < MetadataPagePoolAllocator::PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES; ++i) {
        (void)allocator.allocatePage(Generation(1));
        allocator.reclaimUnreachable(s, {}, false);
        EXPECT_EQ(allocator.stats().allocated_pages,
                  i + 1U == MetadataPagePoolAllocator::PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES ? 0U : i + 1U);
    }
    (void)allocator.allocatePage(Generation(1));
    allocator.reclaimUnreachable(s, {}, false);
    EXPECT_EQ(allocator.stats().allocated_pages, 1U);
    allocator.reclaimUnreachable(s, {});
    EXPECT_EQ(allocator.stats().allocated_pages, 0U);
}
TEST_F(RelocatableMetadataPagePoolTest, BatchedAllocationsAlsoTriggerPublicationReclamation) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    MetadataPagePoolAllocator allocator(io, s); prepareHeadroom(allocator, s);
    (void)allocator.allocatePages(Generation(1), LogSequenceNumber(1), MetadataPagePoolAllocator::PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES, {}, {}, nullptr);
    allocator.reclaimUnreachable(s, {}, false);
    EXPECT_EQ(allocator.stats().allocated_pages, 0U);
}
TEST_F(RelocatableMetadataPagePoolTest, DeferredReclamationAuthenticatesAndPreservesPinnedCatalogRoots) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    MetadataPageFile pages(io, s); MetadataPagePoolAllocator allocator(io, s); prepareHeadroom(allocator, s); CatalogTree tree(pages, allocator);
    const auto old = tree.upsertAndPublish(bootstrap, s, std::vector{poolEntry(100, 2)}, LogSequenceNumber(2));
    const auto current = tree.upsertAndPublish(bootstrap, old, std::vector{poolEntry(101, 3)}, LogSequenceNumber(3));
    (void)allocator.allocatePages(Generation(3), LogSequenceNumber(3), MetadataPagePoolAllocator::PUBLICATION_RECLAIM_ALLOCATION_BATCH_FRAMES, {}, {}, nullptr);
    allocator.reclaimUnreachable(current, std::array{old}, false);
    EXPECT_EQ(tree.find(old, SegmentId(1)).physical_slot, PhysicalSlotId(100));
    EXPECT_EQ(tree.find(current, SegmentId(1)).physical_slot, PhysicalSlotId(101));
    EXPECT_EQ(allocator.stats().allocated_pages, 4U);
    allocator.reclaimUnreachable(current, {});
    EXPECT_EQ(allocator.stats().allocated_pages, 2U);
}
TEST_F(RelocatableMetadataPagePoolTest, DeferredCallStillRejectsFutureSnapshotBeforeThreshold) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    MetadataPagePoolAllocator allocator(io, s); auto future = s; future.generation = Generation(2);
    EXPECT_THROW(allocator.reclaimUnreachable(s, std::array{future}, false), FormatException);
    EXPECT_FALSE(allocator.stats().poisoned);
}
TEST_F(RelocatableMetadataPagePoolTest, RecoveryReclaimsDeferredUnpublishedFrames) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    { MetadataPagePoolAllocator allocator(io, s); prepareHeadroom(allocator, s); (void)allocator.allocatePage(Generation(1));
      allocator.reclaimUnreachable(s, {}, false); EXPECT_EQ(allocator.stats().allocated_pages, 1U); }
    MetadataPagePoolAllocator reopened(io, s);
    EXPECT_EQ(reopened.stats().allocated_pages, 0U);
}

TEST_F(RelocatableMetadataPagePoolTest, LowFreeHeadroomReclaimsBeforeAllocationBatchThreshold) {
    const auto s = initialBlock(); SuperblockFile bootstrap(io); bootstrap.initializeNew(s);
    MetadataPagePoolAllocator allocator(io, s);
    (void)allocator.allocatePage(Generation(1));
    ASSERT_EQ(allocator.stats().allocated_pages, 1U);
    allocator.reclaimUnreachable(s, {}, false);
    EXPECT_EQ(allocator.stats().allocated_pages, 0U);
}
