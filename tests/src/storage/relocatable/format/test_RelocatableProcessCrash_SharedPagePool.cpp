/**
 * @file test_RelocatableProcessCrash_SharedPagePool.cpp
 * @brief Real child-process crash coverage for native shared-page-pool publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_POOL_CONTROL_SHORT_WRITE = "pool-control-short-write";
	constexpr std::string_view CRASH_CHILD_SHARED_POOL_REPLACEMENT_CANDIDATE_SHORT_WRITE =
			"shared-pool-replacement-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE_SHORT_WRITE =
			"shared-pool-evacuation-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE = "shared-pool-evacuation-candidate";
	constexpr std::string_view CRASH_CHILD_SHARED_POOL_RECLAIM_AFTER_PUBLICATION =
			"shared-pool-reclaim-after-publication";

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	SharedPagePoolAllocationRequest allocationRequest(uint64_t segmentId, uint64_t pageId,
													 std::initializer_list<std::byte> payload) {
		SharedPagePoolAllocationRequest request;
		request.segment_id = SegmentId(segmentId);
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = 77U;
		request.generation = Generation(1U);
		request.page_id = PageId(pageId);
		request.page_owner_id = 77U;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		request.payload.assign(payload.begin(), payload.end());
		return request;
	}

	SharedPagePoolReplacementRequest replacementRequest() {
		SharedPagePoolReplacementRequest replacement;
		replacement.segment_id = SegmentId(31U);
		replacement.generation = Generation(1U);
		replacement.page_id = PageId(91U);
		replacement.expected_owner_lsn = LogSequenceNumber(2U);
		replacement.page_owner_id = 77U;
		replacement.page_owner_kind = PageOwnerKind::POK_INDEX;
		replacement.payload = {std::byte{0x21}, std::byte{0x22}};
		return replacement;
	}

	SharedPagePoolEvacuationRequest evacuationRequest() {
		SharedPagePoolEvacuationRequest evacuation;
		evacuation.source_segment_id = SegmentId(31U);
		evacuation.source_generation = Generation(1U);
		evacuation.destination_segment_id = SegmentId(32U);
		evacuation.destination_generation = Generation(1U);
		evacuation.page_id = PageId(91U);
		evacuation.expected_owner_lsn = LogSequenceNumber(2U);
		evacuation.page_owner_id = 77U;
		evacuation.page_owner_kind = PageOwnerKind::POK_INDEX;
		evacuation.payload = {std::byte{0x51}, std::byte{0x52}};
		return evacuation;
	}

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableSharedPagePoolCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																 const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_POOL_CONTROL_SHORT_WRITE &&
			mode != CRASH_CHILD_SHARED_POOL_REPLACEMENT_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE_SHORT_WRITE &&
			mode != CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE &&
			mode != CRASH_CHILD_SHARED_POOL_RECLAIM_AFTER_PUBLICATION) {
			return -1;
		}
		try {
			auto file = std::make_shared<std::fstream>(
					databasePath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			if (!file->is_open()) return 100;
			const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
			if (nativeHandle == INVALID_FILE_HANDLE) return 101;
			auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
			SharedPagePoolAllocator allocator(io, *metadata);
			if (mode == CRASH_CHILD_POOL_CONTROL_SHORT_WRITE) {
				const auto allocation = allocator.allocateNewPoolPage(
						allocationRequest(31U, 91U, {std::byte{0x11}, std::byte{0x12}}), PhysicalSlotId(40U),
						PhysicalSlotId(40U), LogSequenceNumber(2U));
				if (!allocation.has_value()) return 102;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t inactiveControlOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, allocation->pool.physical_slot) +
						RSS_ENVELOPE_BYTES + RSS_SHARED_PAGE_POOL_HEADER_BYTES;
				bool interrupted = false;
				{
					StorageIO::ScopedNativeWriteHookForTesting shortControl(
							[inactiveControlOffset, &interrupted](file_handle_t fd, const void *bytes, size_t byteCount,
																						pwrite_off_t offset) {
								if (static_cast<uint64_t>(offset) == inactiveControlOffset &&
									byteCount == RSS_SHARED_PAGE_POOL_HEADER_BYTES) {
									interrupted = true;
									return portable_pwrite(fd, bytes, 16U, offset);
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					try {
						static_cast<void>(allocator.replacePage(replacementRequest(), LogSequenceNumber(3U)));
					} catch (const std::system_error &) {
						// The marker follows a real partial control-page write.
					}
				}
				if (!interrupted) return 103;
				writeMarker(markerPath);
				waitToBeKilled();
			}
			if (mode == CRASH_CHILD_SHARED_POOL_REPLACEMENT_CANDIDATE_SHORT_WRITE) {
				const auto allocation = allocator.allocateNewPoolPage(
						allocationRequest(31U, 91U, {std::byte{0x11}, std::byte{0x12}}), PhysicalSlotId(40U),
						PhysicalSlotId(40U), LogSequenceNumber(2U));
				if (!allocation.has_value()) return 104;
				MetadataSnapshotPin snapshot = metadata->openSnapshot();
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						snapshot.superblock().data_region_offset, allocation->pool.physical_slot) + 2U * RSS_PAGE_SIZE;
				snapshot.release();
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(allocator.replacePage(replacementRequest(), LogSequenceNumber(3U)));
				return 105;
			}

			const auto source = allocator.allocateNewPoolPage(
					allocationRequest(31U, 91U, {std::byte{0x51}, std::byte{0x52}}), PhysicalSlotId(40U),
					PhysicalSlotId(40U), LogSequenceNumber(2U));
			const auto destination = allocator.allocateNewPoolPage(
					allocationRequest(32U, 92U, {std::byte{0x61}}), PhysicalSlotId(41U), PhysicalSlotId(41U),
					LogSequenceNumber(3U));
			if (!source.has_value() || !destination.has_value()) return 106;
			if (mode == CRASH_CHILD_SHARED_POOL_RECLAIM_AFTER_PUBLICATION) {
				static_cast<void>(allocator.evacuatePage(evacuationRequest(), LogSequenceNumber(4U)));
				const uint64_t sourcePoolOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						initialSuperblock().data_region_offset, source->pool.physical_slot);
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterReclaim(
						[sourcePoolOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
													pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) >= sourcePoolOffset + RSS_ENVELOPE_BYTES &&
								static_cast<uint64_t>(offset) < sourcePoolOffset + RSS_PAGE_SIZE &&
								byteCount == RSS_SHARED_PAGE_POOL_HEADER_BYTES) {
								const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
								if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return persisted;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				allocator.reclaimRetiredFrames(SegmentId(31U));
				return 109;
			}
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
					snapshot.superblock().data_region_offset, destination->pool.physical_slot) + 2U * RSS_PAGE_SIZE;
			snapshot.release();
			StorageIO::ScopedNativeWriteHookForTesting pauseAtCandidate(
					[candidateOffset, mode, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																						 pwrite_off_t offset) {
						if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
							if (mode == CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
							if (partial != 16U) return partial;
							writeMarker(markerPath);
							waitToBeKilled();
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			static_cast<void>(allocator.evacuatePage(evacuationRequest(), LogSequenceNumber(4U)));
			return 107;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 108;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashSharedPagePoolTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
					   ("relocatable_shared_page_pool_process_crash_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

};

TEST_F(RelocatableProcessCrashSharedPagePoolTest, KillAfterPoolControlShortWriteRecoversTheOldPageRoute) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_POOL_CONTROL_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator allocator(io, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const SharedPagePoolContents recovered = allocator.read(snapshot, PageId(91U));
	EXPECT_EQ(recovered.location.page_index, 1U);
	EXPECT_EQ(recovered.payload, (std::vector<std::byte>{std::byte{0x11}, std::byte{0x12}}));
	snapshot.release();
}

TEST_F(RelocatableProcessCrashSharedPagePoolTest,
	   KillDuringSharedPoolReplacementCandidateShortWriteRetainsOldRouteAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SHARED_POOL_REPLACEMENT_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const SharedPagePoolContents old = allocator.read(recovered, PageId(91U));
	EXPECT_EQ(old.location.page_index, 1U);
	EXPECT_EQ(old.payload, (std::vector<std::byte>{std::byte{0x11}, std::byte{0x12}}));
	EXPECT_EQ(old.pool_header.allocated_frames, 1U);
	recovered.release();

	const SharedPagePoolAllocation retried = allocator.replacePage(replacementRequest(), LogSequenceNumber(3U));
	EXPECT_EQ(retried.location.page_index, 2U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(allocator.read(published, PageId(91U)).payload,
			  (std::vector<std::byte>{std::byte{0x21}, std::byte{0x22}}));
	published.release();
}

TEST_F(RelocatableProcessCrashSharedPagePoolTest,
	   KillDuringSharedPoolEvacuationCandidateShortWriteRetainsSourceAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const SharedPagePoolContents source = allocator.read(recovered, PageId(91U));
	EXPECT_EQ(source.pool.segment_id, SegmentId(31U));
	EXPECT_EQ(source.payload, (std::vector<std::byte>{std::byte{0x51}, std::byte{0x52}}));
	EXPECT_EQ(allocator.read(recovered, PageId(92U)).pool_header.allocated_frames, 1U);
	recovered.release();

	const SharedPagePoolAllocation retried = allocator.evacuatePage(evacuationRequest(), LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.page_index, 2U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(allocator.read(published, PageId(91U)).pool.segment_id, SegmentId(32U));
	published.release();
}

TEST_F(RelocatableProcessCrashSharedPagePoolTest, KillDuringSharedPoolEvacuationCandidateRetainsSourceAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SHARED_POOL_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const SharedPagePoolContents source = allocator.read(recovered, PageId(91U));
	EXPECT_EQ(source.pool.segment_id, SegmentId(31U));
	EXPECT_EQ(source.payload, (std::vector<std::byte>{std::byte{0x51}, std::byte{0x52}}));
	EXPECT_EQ(allocator.read(recovered, PageId(92U)).pool_header.allocated_frames, 1U);
	recovered.release();

	const SharedPagePoolAllocation retried = allocator.evacuatePage(evacuationRequest(), LogSequenceNumber(4U));
	EXPECT_EQ(retried.location.page_index, 2U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(allocator.read(published, PageId(91U)).pool.segment_id, SegmentId(32U));
	published.release();
}

TEST_F(RelocatableProcessCrashSharedPagePoolTest,
		KillAfterSharedPoolReclaimPublicationKeepsTheMovedPageReadableAndSourceFrameReusable) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_SHARED_POOL_RECLAIM_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SharedPagePoolAllocator allocator(io, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const CatalogEntry sourcePool = metadata->findCatalog(recovered, SegmentId(31U));
	EXPECT_EQ(allocator.read(recovered, PageId(91U)).pool.segment_id, SegmentId(32U));
	EXPECT_EQ(allocator.read(recovered, PageId(91U)).payload,
			  (std::vector<std::byte>{std::byte{0x51}, std::byte{0x52}}));
	EXPECT_EQ(allocator.availableFrameCount(sourcePool), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	recovered.release();

	SharedPagePoolAppendRequest reuseRequest;
	reuseRequest.segment_id = SegmentId(31U);
	reuseRequest.generation = Generation(1U);
	reuseRequest.page_id = PageId(93U);
	reuseRequest.page_owner_id = 77U;
	reuseRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	const SharedPagePoolAllocation reused = allocator.appendPage(reuseRequest, LogSequenceNumber(5U));
	EXPECT_EQ(reused.location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(allocator.read(published, PageId(93U)).pool.segment_id, SegmentId(31U));
}
