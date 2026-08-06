/**
 * @file test_RelocatableTailTruncation.cpp
 * @brief Snapshot-safe physical tail truncation tests for relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	PhysicalSegmentAllocationRequest segmentRequest() {
		PhysicalSegmentAllocationRequest result;
		result.segment_id = SegmentId(71);
		result.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
		result.owner_id = 17;
		result.generation = Generation(1);
		return result;
	}

	PageOwnerHeader propertyOwner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId, 99, PageOwnerKind::POK_PROPERTY, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1), lsn};
	}

	SlottedPagePoolAllocationRequest propertyRequest(SegmentId segmentId, PageId pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = 99;
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = 99;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	VectorCodebookWriteRequest codebookRequest() {
		VectorCodebookWriteRequest result;
		result.codebook_id = 17;
		result.manifest_segment_id = SegmentId(81);
		result.generation = Generation(1);
		result.element_type = VectorCodebookElementType::VCET_FLOAT32;
		result.dimensions = 2;
		result.subspace_count = 1;
		result.centroid_count = 3;
		return result;
	}

} // namespace

class RelocatableTailTruncationTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;
	file_handle_t nativeFd = INVALID_FILE_HANDLE;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_tail_truncation_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
	}

	void TearDown() override {
		io.reset();
		if (nativeFd != INVALID_FILE_HANDLE) {
			static_cast<void>(portable_close_rw(nativeFd));
		}
		if (file && file->is_open()) {
			file->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};

TEST_F(RelocatableTailTruncationTest, TruncatesOnlyReleasedTailSlotsAfterSnapshotsDrainAndReopens) {
	nativeFd = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	ASSERT_TRUE(segments.allocate(segmentRequest(), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	static_cast<void>(segments.retire(SegmentId(71), Generation(1), LogSequenceNumber(3)));
	static_cast<void>(segments.releaseRetired(SegmentId(71), Generation(1), LogSequenceNumber(4)));
	const uint64_t originalBytes = io->fileSize();

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	EXPECT_EQ(metadata->truncateReleasedTail().status, RelocatableTailTruncationStatus::VTTS_AWAITING_SNAPSHOTS);
	pinned.release();
	const RelocatableTailTruncationResult result = metadata->truncateReleasedTail();
	EXPECT_EQ(result.status, RelocatableTailTruncationStatus::VTTS_TRUNCATED);
	EXPECT_EQ(result.old_file_bytes, originalBytes);
	EXPECT_LT(result.new_file_bytes, result.old_file_bytes);
	EXPECT_EQ(io->fileSize(), result.new_file_bytes);
	EXPECT_GE(result.reclaimed_slots, 1U);
	EXPECT_EQ(metadata->truncateReleasedTail().status, RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	const MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(snapshot, SegmentId(71)).state, SegmentState::SGS_OBSOLETE);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableTailTruncationTest, RefusesFallbackOnlyStorageWithoutRiskingTheFileTail) {
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	PhysicalSegmentAllocator segments(io, *metadata);
	ASSERT_TRUE(segments.allocate(segmentRequest(), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	static_cast<void>(segments.retire(SegmentId(71), Generation(1), LogSequenceNumber(3)));
	static_cast<void>(segments.releaseRetired(SegmentId(71), Generation(1), LogSequenceNumber(4)));
	const uint64_t originalBytes = io->fileSize();
	const RelocatableTailTruncationResult result = metadata->truncateReleasedTail();
	EXPECT_EQ(result.status, RelocatableTailTruncationStatus::VTTS_NATIVE_IO_REQUIRED);
	EXPECT_EQ(io->fileSize(), originalBytes);
}

TEST_F(RelocatableTailTruncationTest, TruncatesAReleasedExtentTailAfterMixedPageAndExtentRelocationThenReopens) {
	nativeFd = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeFd, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeFd, nativeFd);
	const std::vector<std::byte> expectedCodebook(24U, std::byte{0x21});
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		SlottedPagePool pages(io, *metadata);
		SlottedPage sourcePage = SlottedPage::create(propertyOwner(PageId(101), LogSequenceNumber(2)));
		ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(71), PageId(101)), sourcePage, PhysicalSlotId(40),
									   PhysicalSlotId(40), LogSequenceNumber(2))
							.has_value());
		SlottedPage destinationPage = SlottedPage::create(propertyOwner(PageId(102), LogSequenceNumber(4)));
		ASSERT_TRUE(pages.allocatePage(propertyRequest(SegmentId(72), PageId(102)), destinationPage, PhysicalSlotId(41),
									   PhysicalSlotId(41), LogSequenceNumber(4))
							.has_value());

		PhysicalSegmentAllocator physical(io, *metadata);
		VectorCodebookStore codebooks(physical, *metadata);
		static_cast<void>(codebooks.writeImmutable(codebookRequest(), expectedCodebook, PhysicalSlotId(43),
												   PhysicalSlotId(43), LogSequenceNumber(5)));
		RelocatableCompactionCoordinator pageCoordinator(pages, *metadata);
		const RelocatableSlottedPoolCompactionResult pageResult =
				pageCoordinator.compactSlottedPool({SegmentId(71), SegmentId(72), 1, LogSequenceNumber(6)});
		ASSERT_EQ(pageResult.status, RelocatableCompactionStatus::VCS_COMPLETED);
		ASSERT_TRUE(pageResult.source_released);
		RelocatableCompactionCoordinator extentCoordinator(pages, codebooks, physical, *metadata);
		const RelocatableExtentCompactionResult extentResult = extentCoordinator.compactCodebookExtent(
				{17, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(9)});
		ASSERT_EQ(extentResult.status, RelocatableCompactionStatus::VCS_COMPLETED);
		ASSERT_TRUE(extentResult.source_released);

		MetadataSnapshotPin beforeTruncation = metadata->openSnapshot();
		EXPECT_EQ(metadata->findPageLocation(beforeTruncation, PageId(101)).segment_id, SegmentId(72));
		EXPECT_EQ(codebooks.read(beforeTruncation, 17).payload, expectedCodebook);
		EXPECT_EQ(metadata->findCatalog(beforeTruncation, SegmentId(81)).physical_slot, PhysicalSlotId(42));
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(beforeTruncation, PhysicalSlotId(43)));
		beforeTruncation.release();

		const uint64_t originalBytes = io->fileSize();
		const RelocatableTailTruncationResult truncated = metadata->truncateReleasedTail();
		EXPECT_EQ(truncated.status, RelocatableTailTruncationStatus::VTTS_TRUNCATED);
		EXPECT_EQ(truncated.old_file_bytes, originalBytes);
		EXPECT_LT(truncated.new_file_bytes, originalBytes);
		EXPECT_GE(truncated.reclaimed_slots, 1U);
		EXPECT_EQ(io->fileSize(), truncated.new_file_bytes);
	}

	auto reopened = RelocatableMetadataStore::open(io);
	PhysicalSegmentAllocator reopenedPhysical(io, *reopened);
	VectorCodebookStore reopenedCodebooks(reopenedPhysical, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(snapshot, PageId(101)).segment_id, SegmentId(72));
	EXPECT_EQ(reopenedCodebooks.read(snapshot, 17).payload, expectedCodebook);
	EXPECT_EQ(reopened->findCatalog(snapshot, SegmentId(81)).physical_slot, PhysicalSlotId(42));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(42)));
}
