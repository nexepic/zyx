#include <algorithm>
#include "storage/relocatable/metadata/MetadataPagePoolTestFixture.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"

using namespace graph::storage;
using graph::storage::test_support::RelocatableMetadataPagePoolTest;

TEST_F(RelocatableMetadataPagePoolTest, PartialCapacityFailureRollsBackAllCreatedPoolsAndCanRetry) {
    io.reset(); file->close(); file.reset();
    const auto handle = portable_open_rw(testFilePath.string().c_str());
    ASSERT_NE(handle, INVALID_FILE_HANDLE);
    struct Close { file_handle_t handle; ~Close() { portable_close_rw(handle); } } close{handle};
    Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1); root.checkpoint_lsn = LogSequenceNumber(1);
    auto limited = std::make_shared<StorageIO>(nullptr, handle, handle, StorageIOAccessMode::SIOAM_READ_WRITE,
        root.data_region_offset + RSS_SEGMENT_SIZE);
    SuperblockFile(limited).initializeNew(root);
    MetadataPagePoolAllocator allocator(limited, root);
    allocator.beginUnpublishedBatch(root);
    const auto before = limited->fileSize();
    const auto oldPools = allocator.poolSlots();
    EXPECT_THROW(static_cast<void>(allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        2U * RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr)), StorageFileLimitExceeded);
    EXPECT_EQ(allocator.poolSlots(), oldPools);
    EXPECT_EQ(limited->fileSize(), before);
    EXPECT_EQ(SuperblockFile(limited).recover(), root);
    std::vector<PhysicalSlotId> created;
    ASSERT_NO_THROW(created = allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr));
    ASSERT_EQ(created.size(), 1U);
    EXPECT_TRUE(allocator.isMetadataPoolSlot(created.front()));
    EXPECT_EQ(allocator.poolSlots().size(), oldPools.size() + 1U);
}

TEST_F(RelocatableMetadataPagePoolTest, CapacityFailurePreservesExistingPoolFramesAndAllocatorHealth) {
    io.reset(); file->close(); file.reset();
    const auto handle = portable_open_rw(testFilePath.string().c_str());
    ASSERT_NE(handle, INVALID_FILE_HANDLE);
    struct Close { file_handle_t handle; ~Close() { portable_close_rw(handle); } } close{handle};
    Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1); root.checkpoint_lsn = LogSequenceNumber(1);
    auto limited = std::make_shared<StorageIO>(nullptr, handle, handle, StorageIOAccessMode::SIOAM_READ_WRITE,
        root.data_region_offset + 2U * RSS_SEGMENT_SIZE);
    SuperblockFile(limited).initializeNew(root);
    MetadataPagePoolAllocator allocator(limited, root);
    const auto original = allocator.allocatePages(Generation(1), LogSequenceNumber(1),
        RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr);
    ASSERT_EQ(original.size(), RSS_PAGE_POOL_DATA_FRAME_COUNT);
    const auto oldPools = allocator.poolSlots();
    const auto header = allocator.poolHeader(original.front().page.physical_slot);
    const auto before = limited->fileSize();
    allocator.beginUnpublishedBatch(root);
    EXPECT_THROW(static_cast<void>(allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        2U * RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr)), StorageFileLimitExceeded);
    EXPECT_EQ(allocator.poolSlots(), oldPools);
    const auto after = allocator.poolHeader(original.front().page.physical_slot);
    EXPECT_EQ(after.allocated_frames, header.allocated_frames);
    EXPECT_EQ(after.segment_id, header.segment_id); EXPECT_EQ(after.generation, header.generation);
    EXPECT_EQ(after.committed_lsn, header.committed_lsn); EXPECT_EQ(after.control_sequence, header.control_sequence);
    EXPECT_EQ(limited->fileSize(), before);
    std::vector<PhysicalSlotId> created;
    ASSERT_NO_THROW(created = allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr));
    ASSERT_EQ(created.size(), 1U);
    EXPECT_NE(created.front(), original.front().page.physical_slot);
}

TEST_F(RelocatableMetadataPagePoolTest, FailedCapacityDoesNotLeaveARegisteredPoolInADurableFreeHole) {
    io.reset(); file->close(); file.reset();
    const auto handle = portable_open_rw(testFilePath.string().c_str());
    ASSERT_NE(handle, INVALID_FILE_HANDLE);
    struct Close { file_handle_t handle; ~Close() { portable_close_rw(handle); } } close{handle};
    Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1); root.checkpoint_lsn = LogSequenceNumber(1);
    auto limited = std::make_shared<StorageIO>(nullptr, handle, handle, StorageIOAccessMode::SIOAM_READ_WRITE,
        root.data_region_offset + RSS_SEGMENT_SIZE);
    SuperblockFile(limited).initializeNew(root);
    ASSERT_EQ(limited->reserveAppendSpace(RSS_SEGMENT_SIZE), root.data_region_offset);
    MetadataPagePoolAllocator allocator(limited, root);
    allocator.beginUnpublishedBatch(root);
    const auto before = limited->fileSize();
    EXPECT_THROW(static_cast<void>(allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        2U * RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr)), StorageFileLimitExceeded);
    EXPECT_TRUE(allocator.poolSlots().empty()); EXPECT_EQ(limited->fileSize(), before);
    std::array<std::byte, RSS_PAGE_SIZE> control{};
    ASSERT_EQ(limited->readAt(root.data_region_offset, control.data(), control.size()), control.size());
    EXPECT_TRUE(std::all_of(control.begin(), control.end(), [](std::byte value) { return value == std::byte{}; }));
    std::vector<PhysicalSlotId> created;
    ASSERT_NO_THROW(created = allocator.ensureMetadataFrameCapacity(Generation(2), LogSequenceNumber(2),
        RSS_PAGE_POOL_DATA_FRAME_COUNT, {}, {}, nullptr));
    ASSERT_EQ(created.size(), 1U); EXPECT_EQ(created.front(), PhysicalSlotId(1));
}
