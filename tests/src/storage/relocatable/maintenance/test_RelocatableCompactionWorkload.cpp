/**
 * @file test_RelocatableCompactionWorkload.cpp
 * @brief Deterministic reader/writer/snapshot compaction workload for relocatable storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <latch>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableCompactionCoordinator.hpp"
#include "storage/relocatable/TestSlottedPageRecord.hpp"

using namespace graph::storage;
using graph::storage::test::TestSlottedPageRecord;
using graph::storage::test::TestSlottedPageRecordCodec;

namespace {

	constexpr uint64_t SOURCE_SEGMENT_ID = 71;
	constexpr uint64_t DESTINATION_SEGMENT_ID = 72;
	constexpr uint64_t SOURCE_OWNER_ID = 99;
	constexpr uint64_t FIRST_SOURCE_PAGE_ID = 101;
	constexpr uint64_t DESTINATION_PAGE_ID = 200;
	constexpr size_t WORKLOAD_PAGE_COUNT = 12;

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	PageOwnerHeader propertyOwner(PageId pageId, LogSequenceNumber lsn) {
		return {pageId,
				SOURCE_OWNER_ID,
				PageOwnerKind::POK_PROPERTY,
				0,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1),
				lsn};
	}

	SlottedPagePoolAllocationRequest sourceRequest(PageId pageId) {
		SlottedPagePoolAllocationRequest result;
		result.segment_id = SegmentId(SOURCE_SEGMENT_ID);
		result.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
		result.pool_owner_id = SOURCE_OWNER_ID;
		result.generation = Generation(1);
		result.page_id = pageId;
		result.page_owner_id = SOURCE_OWNER_ID;
		result.page_owner_kind = PageOwnerKind::POK_PROPERTY;
		return result;
	}

	SlottedPagePoolAllocationRequest destinationRequest() {
		SlottedPagePoolAllocationRequest result = sourceRequest(PageId(DESTINATION_PAGE_ID));
		result.segment_id = SegmentId(DESTINATION_SEGMENT_ID);
		return result;
	}

	TestSlottedPageRecord testRecord(uint64_t logicalId) {
		TestSlottedPageRecord result;
		result.logical_id = logicalId;
		result.payload = {std::byte{static_cast<unsigned char>(logicalId)}, std::byte{0xA5}};
		return result;
	}

} // namespace

class RelocatableCompactionWorkloadTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;
	file_handle_t nativeHandle{INVALID_FILE_HANDLE};

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_compaction_workload_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	}

	void TearDown() override {
		io.reset();
		if (nativeHandle != INVALID_FILE_HANDLE) {
			EXPECT_EQ(portable_close_rw(nativeHandle), 0);
			nativeHandle = INVALID_FILE_HANDLE;
		}
		if (file && file->is_open()) {
			file->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};

TEST_F(RelocatableCompactionWorkloadTest, BoundsRetiredGenerationsAndSpaceAcrossDeterministicReaderWriterTicks) {
	std::array<TestSlottedPageRecord, WORKLOAD_PAGE_COUNT> expected{};
	std::array<SlottedSlotReference, WORKLOAD_PAGE_COUNT> slots{};
	uint64_t nextLsn = 2;
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		SlottedPagePool pages(io, *metadata);
		for (size_t index = 0; index < WORKLOAD_PAGE_COUNT; ++index) {
			const PageId pageId(FIRST_SOURCE_PAGE_ID + index);
			expected[index] = testRecord(index + 1U);
			SlottedPage page = SlottedPage::create(propertyOwner(pageId, LogSequenceNumber(nextLsn)));
			slots[index] = page.insert(TestSlottedPageRecordCodec::encode(expected[index]));
			if (index == 0U) {
				ASSERT_TRUE(pages.allocatePage(sourceRequest(pageId), page, PhysicalSlotId(40), PhysicalSlotId(40),
											   LogSequenceNumber(nextLsn))
									.has_value());
			} else {
				SlottedPagePoolAppendRequest append;
				append.segment_id = SegmentId(SOURCE_SEGMENT_ID);
				append.generation = Generation(1);
				append.page_id = pageId;
				append.page_owner_id = SOURCE_OWNER_ID;
				append.page_owner_kind = PageOwnerKind::POK_PROPERTY;
				static_cast<void>(pages.appendPage(append, page, LogSequenceNumber(nextLsn)));
			}
			++nextLsn;
		}
		SlottedPage destination =
				SlottedPage::create(propertyOwner(PageId(DESTINATION_PAGE_ID), LogSequenceNumber(nextLsn)));
		ASSERT_TRUE(pages.allocatePage(destinationRequest(), destination, PhysicalSlotId(41), PhysicalSlotId(41),
									   LogSequenceNumber(nextLsn))
							.has_value());
		++nextLsn;

		RelocatableCompactionCoordinator coordinator(pages, *metadata);
		uint64_t maximumAllocatedSlots = 0;
		for (size_t index = 0; index < WORKLOAD_PAGE_COUNT; ++index) {
			MetadataSnapshotPin reader = metadata->openSnapshot();
			const PageId sourcePageId(FIRST_SOURCE_PAGE_ID + index);
			EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(reader, sourcePageId).read(slots[index])),
					  expected[index]);

			MetadataSnapshotPin writer = metadata->openSnapshot();
			SlottedPage updatedDestination = pages.read(writer, PageId(DESTINATION_PAGE_ID));
			const std::array writerValue = {std::byte{static_cast<unsigned char>(index)}, std::byte{0x5C}};
			static_cast<void>(updatedDestination.insert(writerValue));
			writer.release();
			static_cast<void>(pages.replacePage(updatedDestination, LogSequenceNumber(nextLsn++)));

			const RelocatableSlottedPoolCompactionResult moved = coordinator.compactSlottedPool(
					{SegmentId(SOURCE_SEGMENT_ID), SegmentId(DESTINATION_SEGMENT_ID), 1, LogSequenceNumber(nextLsn)});
			nextLsn = moved.next_lsn.value();
			EXPECT_EQ(moved.moved_pages, 1U);
			if (index + 1U == WORKLOAD_PAGE_COUNT) {
				EXPECT_EQ(moved.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
				EXPECT_TRUE(moved.source_retired);
				EXPECT_FALSE(moved.source_released);
			} else {
				EXPECT_EQ(moved.status, RelocatableCompactionStatus::VCS_COMPLETED);
				EXPECT_FALSE(moved.source_retired);
			}
			EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(reader, sourcePageId).read(slots[index])),
					  expected[index]);
			reader.release();
			if (index + 1U != WORKLOAD_PAGE_COUNT) {
				pages.reclaimRetiredFrames(SegmentId(SOURCE_SEGMENT_ID));
			}
			pages.reclaimRetiredFrames(SegmentId(DESTINATION_SEGMENT_ID));

			MetadataSnapshotPin current = metadata->openSnapshot();
			const uint64_t allocatedSlots =
					static_cast<uint64_t>(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40))) +
					static_cast<uint64_t>(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(41)));
			maximumAllocatedSlots = std::max(maximumAllocatedSlots, allocatedSlots);
			current.release();
			EXPECT_LE(metadata->observeCompactionState().retainedGenerationCount(), 1U);
		}

		ASSERT_TRUE(coordinator.releaseRetiredSlottedPool(SegmentId(SOURCE_SEGMENT_ID), Generation(1),
														  LogSequenceNumber(nextLsn++)));
		const MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findCatalog(current, SegmentId(SOURCE_SEGMENT_ID)).state, SegmentState::SGS_OBSOLETE);
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(41)));
		EXPECT_LE(maximumAllocatedSlots, 2U);
		EXPECT_EQ(metadata->observeCompactionState().retainedGenerationCount(), 0U);
	}

	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	for (size_t index = 0; index < WORKLOAD_PAGE_COUNT; ++index) {
		const PageId pageId(FIRST_SOURCE_PAGE_ID + index);
		EXPECT_EQ(reopened->findPageLocation(snapshot, pageId).segment_id, SegmentId(DESTINATION_SEGMENT_ID));
		EXPECT_EQ(TestSlottedPageRecordCodec::decode(reopenedPages.read(snapshot, pageId).read(slots[index])),
				  expected[index]);
	}
}

TEST_F(RelocatableCompactionWorkloadTest, SerializesLiveReaderWriterAndCompactorThreadsWithoutReusingPinnedFrames) {
	const TestSlottedPageRecord expected = testRecord(1);
	const std::vector<std::byte> writerValue = {std::byte{0x5E}, std::byte{0x7F}};
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool pages(io, *metadata);
	SlottedPage source = SlottedPage::create(propertyOwner(PageId(FIRST_SOURCE_PAGE_ID), LogSequenceNumber(2)));
	const SlottedSlotReference sourceSlot = source.insert(TestSlottedPageRecordCodec::encode(expected));
	ASSERT_TRUE(pages.allocatePage(sourceRequest(PageId(FIRST_SOURCE_PAGE_ID)), source, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	SlottedPage destination = SlottedPage::create(propertyOwner(PageId(DESTINATION_PAGE_ID), LogSequenceNumber(3)));
	ASSERT_TRUE(pages.allocatePage(destinationRequest(), destination, PhysicalSlotId(41), PhysicalSlotId(41),
								   LogSequenceNumber(3))
						.has_value());

	RelocatableCompactionCoordinator coordinator(pages, *metadata);
	std::latch readerPinned{1};
	std::latch writerPublished{1};
	std::latch compactionPublished{1};
	std::latch readerReleased{1};
	std::exception_ptr workerFailure;
	std::mutex failureMutex;
	const auto recordFailure = [&](std::exception_ptr error) {
		std::lock_guard lock(failureMutex);
		if (!workerFailure) {
			workerFailure = std::move(error);
		}
	};
	std::atomic<bool> initialRead{false};
	std::atomic<bool> retainedRead{false};
	std::atomic<bool> sourceReleased{false};
	SlottedSlotReference destinationSlot;
	RelocatableSlottedPoolCompactionResult compaction;

	std::thread reader([&] {
		bool announcedPin = false;
		try {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			initialRead.store(TestSlottedPageRecordCodec::decode(
									  pages.read(snapshot, PageId(FIRST_SOURCE_PAGE_ID)).read(sourceSlot)) == expected,
							  std::memory_order_relaxed);
			readerPinned.count_down();
			announcedPin = true;
			compactionPublished.wait();
			retainedRead.store(TestSlottedPageRecordCodec::decode(
									   pages.read(snapshot, PageId(FIRST_SOURCE_PAGE_ID)).read(sourceSlot)) == expected,
							   std::memory_order_relaxed);
			snapshot.release();
		} catch (...) {
			recordFailure(std::current_exception());
		}
		if (!announcedPin) {
			readerPinned.count_down();
		}
		readerReleased.count_down();
	});

	std::thread writer([&] {
		readerPinned.wait();
		try {
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			SlottedPage updatedDestination = pages.read(snapshot, PageId(DESTINATION_PAGE_ID));
			destinationSlot = updatedDestination.insert(writerValue);
			snapshot.release();
			static_cast<void>(pages.replacePage(updatedDestination, LogSequenceNumber(4)));
		} catch (...) {
			recordFailure(std::current_exception());
		}
		writerPublished.count_down();
	});

	std::thread compactor([&] {
		readerPinned.wait();
		writerPublished.wait();
		try {
			compaction = coordinator.compactSlottedPool(
					{SegmentId(SOURCE_SEGMENT_ID), SegmentId(DESTINATION_SEGMENT_ID), 1, LogSequenceNumber(5)});
		} catch (...) {
			recordFailure(std::current_exception());
		}
		compactionPublished.count_down();
		readerReleased.wait();
		try {
			if (compaction.source_retired) {
				sourceReleased.store(coordinator.releaseRetiredSlottedPool(SegmentId(SOURCE_SEGMENT_ID), Generation(1),
																		   compaction.next_lsn),
									 std::memory_order_relaxed);
			}
		} catch (...) {
			recordFailure(std::current_exception());
		}
	});

	reader.join();
	writer.join();
	compactor.join();

	ASSERT_FALSE(static_cast<bool>(workerFailure));
	EXPECT_TRUE(initialRead.load(std::memory_order_relaxed));
	EXPECT_TRUE(retainedRead.load(std::memory_order_relaxed));
	EXPECT_EQ(compaction.status, RelocatableCompactionStatus::VCS_SOURCE_RETIRED_AWAITING_SNAPSHOTS);
	EXPECT_TRUE(compaction.source_retired);
	EXPECT_FALSE(compaction.source_released);
	EXPECT_TRUE(sourceReleased.load(std::memory_order_relaxed));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(FIRST_SOURCE_PAGE_ID)).segment_id,
			  SegmentId(DESTINATION_SEGMENT_ID));
	EXPECT_EQ(TestSlottedPageRecordCodec::decode(pages.read(current, PageId(FIRST_SOURCE_PAGE_ID)).read(sourceSlot)),
			  expected);
	EXPECT_EQ(pages.read(current, PageId(DESTINATION_PAGE_ID)).read(destinationSlot), writerValue);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(40)));
	EXPECT_TRUE(metadata->isPhysicalSlotAllocated(current, PhysicalSlotId(41)));
	current.release();
}
